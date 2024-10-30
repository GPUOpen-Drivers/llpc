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

//===- SpecializeDriverShaders.cpp - Specialize driver shaders based on full-pipeline knowledge -------------------===//

#include "llvmraytracing/SpecializeDriverShaders.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/ValueOriginTracking.h"
#include "compilerutils/ValueSpecialization.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/IR/Module.h"
#include <cassert>

using namespace llvm;
using namespace CompilerUtils;

#define DEBUG_TYPE "specialize-driver-shaders"
// Normal debug output that is also used in testing is wrapped in LLVM_DEBUG
// which can be enabled with --debug arguments.
//
// Even more detailed debug output is wrapped in DETAIL_DEBUG which can be enabled by changing EnableDetailDebugOutput.
// This can be useful when debugging, for instance why a particular argument slot was not detected as preserved.
static constexpr bool EnableDetailDebugOutput = false;
#define DETAIL_DEBUG(BODY)                                                                                             \
  LLVM_DEBUG({                                                                                                         \
    if (EnableDetailDebugOutput) {                                                                                     \
      BODY;                                                                                                            \
    }                                                                                                                  \
  })

namespace {

namespace MsgPackFormat {

constexpr unsigned MajorVersion = 1;

static constexpr char Version[] = "version";
static constexpr char TraversalArgsInfo[] = "traversal_args_info";

} // namespace MsgPackFormat

namespace MetadataFormat {

// For metadata, we don't need to safeguard against version mismatches,
// as metadata is only used temporarily within modules and not stored to disk,
// so every metadata we deserialize has been serialized by the same version of ourselves.
//
// We use an `lgc.rt` prefix even though this is not officially part of the lgc.rt dialect to indicate this is part
// of llvmraytracing. It is however private metadata of this pass and not accessed elsewhere.
static constexpr char State[] = "lgc.rt.specialize.driver.shaders.state";
static constexpr char Options[] = "lgc.rt.specialize.driver.shaders.opts";

} // namespace MetadataFormat

namespace MDHelper {
std::optional<uint32_t> extractZExtI32Constant(Metadata *MD) {
  if (MD) {
    uint64_t Result = mdconst::extract<ConstantInt>(MD)->getZExtValue();
    assert(Result <= std::numeric_limits<uint32_t>::max());
    return Result;
  }
  return std::nullopt;
}

Metadata *getI32MDConstant(LLVMContext &Context, uint32_t Value) {
  IntegerType *Int32Ty = Type::getInt32Ty(Context);
  Metadata *Result = ConstantAsMetadata::get(ConstantInt::get(Int32Ty, Value));
  assert(Result && "Failed to create metadata node!");
  assert(extractZExtI32Constant(Result) == Value && "Failed to extract value from node!");
  return Result;
}

} // namespace MDHelper

// Utilities to keep track of the "status" of individual arg slots.
// There is some similarity between these pairs of types:
//  * ArgSlotStatus and ValueTracking::SliceStatus
//  * ArgSlotInfo and ValueTracking::SliceInfo
//  * ArgSlotsInfo and ValueTracking::ValueInfo
//
// The main difference is due to the notion of "Preserved" arguments,
// which doesn't make sense for general values, and due to the fact
// that we don't care about the contents of (non-preserved) dynamic arguments.
// Also, we don't support bitmasks of multiple possible status, and instead
// treat multi-status cases conservatively.
enum class ArgSlotStatus : uint32_t {
  Dynamic = 0,   // The arg slot is set to an unknown value and does not preserve the corresponding incoming arg slot.
  Constant,      // The arg slot is set to a known constant
  UndefOrPoison, // The arg slot is undef or poison
  Preserve,      // The arg slot preserves the corresponding incoming arg slot.
                 //  Only used for in-Traversal functions, like Traversal or AHS,
                 //  but not for jumps from non-Traversal functions to Traversal functions (e.g. TraceRay call sites).
  Count
};

StringRef toString(ArgSlotStatus AS, bool Compact = false) {
  switch (AS) {
  case ArgSlotStatus::Dynamic:
    return Compact ? "D" : "Dynamic";
  case ArgSlotStatus::Constant:
    return Compact ? "C" : "Constant";
  case ArgSlotStatus::UndefOrPoison:
    return Compact ? "U" : "UndefOrPoison";
  case ArgSlotStatus::Preserve:
    return Compact ? "P" : "Preserve";
  default:
    break;
  }
  report_fatal_error("Unexpected value " + Twine(static_cast<int>(AS)));
}

[[maybe_unused]] llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ArgSlotStatus &AS) {
  OS << toString(AS);
  return OS;
}

// This is stored for every incoming arg slot and every function we'd like to specialize.
struct ArgSlotInfo {
  ArgSlotStatus Status = ArgSlotStatus::Dynamic;
  uint32_t ConstantValue = 0;

  void print(llvm::raw_ostream &OS, bool Compact = false) const {
    OS << toString(Status, Compact);
    if (!Compact && Status == ArgSlotStatus::Constant) {
      OS << "=0x";
      OS.write_hex(ConstantValue);
    }
  }

  static ArgSlotInfo combine(const ArgSlotInfo &LHS, const ArgSlotInfo &RHS) {
    if (LHS.Status == ArgSlotStatus::Preserve)
      return RHS;
    if (RHS.Status == ArgSlotStatus::Preserve)
      return LHS;

    if (LHS.Status == ArgSlotStatus::Dynamic || RHS.Status == ArgSlotStatus::Dynamic)
      return {ArgSlotStatus::Dynamic};

    // Both are undef or constant. Merge Undef + constant -> constant
    // If we wanted to treat poison/undef as constant zero instead, this is the place we'd need to change.
    if (LHS.Status == ArgSlotStatus::UndefOrPoison)
      return RHS;
    if (RHS.Status == ArgSlotStatus::UndefOrPoison)
      return LHS;

    assert(LHS.Status == ArgSlotStatus::Constant && RHS.Status == ArgSlotStatus::Constant);
    if (LHS.ConstantValue == RHS.ConstantValue)
      return LHS;

    return {ArgSlotStatus::Dynamic};
  }

  bool operator==(const ArgSlotInfo &Other) const {
    return std::tie(Status, ConstantValue) == std::tie(Other.Status, Other.ConstantValue);
  }
  bool operator!=(const ArgSlotInfo &Other) const { return !(*this == Other); }
};

[[maybe_unused]] llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ArgSlotInfo &AS) {
  AS.print(OS);
  return OS;
}

// Infos about all relevant arg slots of a function.
struct ArgSlotsInfo {
  SmallVector<ArgSlotInfo> ArgSlots;

  static llvm::Expected<ArgSlotsInfo> decodeMsgpack(llvm::msgpack::DocNode &Node) {
    // Format: Array of Status/ConstantValue pairs.
    auto &ArrNode = Node.getArray();
    if (ArrNode.size() % 2)
      return make_error<StringError>("expected even array length", inconvertibleErrorCode());

    ArgSlotsInfo Result{};
    Result.ArgSlots.resize(ArrNode.size() / 2);
    for (unsigned Idx = 0; Idx < Result.ArgSlots.size(); ++Idx) {
      auto &StatusNode = ArrNode[2 * Idx];
      auto &ConstantValueNode = ArrNode[2 * Idx + 1];
      if (StatusNode.isEmpty() || ConstantValueNode.isEmpty())
        return make_error<StringError>("unexpected empty nodes", inconvertibleErrorCode());
      ArgSlotStatus Status = static_cast<ArgSlotStatus>(StatusNode.getUInt());
      if (Status >= ArgSlotStatus::Count)
        return make_error<StringError>("invalid status", inconvertibleErrorCode());
      uint32_t ConstantValue = ConstantValueNode.getUInt();
      Result.ArgSlots[Idx] = {Status, ConstantValue};
    }
    return Result;
  }

  void encodeMsgpack(llvm::msgpack::DocNode &Node) const {
    auto &ArrNode = Node.getArray(true);
    unsigned Idx = 0;
    for (const ArgSlotInfo &ASI : ArgSlots) {
      // Serialize ArgSlotInfo using two 32-bit values: The first one gives
      // the status, the second one the constant (if there is one)
      ArrNode[Idx++] = static_cast<uint32_t>(ASI.Status);
      ArrNode[Idx++] = ASI.ConstantValue;
    }
  }

  static llvm::Expected<ArgSlotsInfo> fromMetadata(const llvm::MDNode *MD) {
    unsigned NumMDOperands = MD->getNumOperands();
    if (NumMDOperands % 2)
      return make_error<StringError>("expected even array length", inconvertibleErrorCode());
    unsigned NumArgs = NumMDOperands / 2;
    ArgSlotsInfo Result{};
    Result.ArgSlots.resize(NumArgs);
    for (unsigned Idx = 0; Idx < NumArgs; ++Idx) {
      auto OptStatus = MDHelper::extractZExtI32Constant(MD->getOperand(2 * Idx));
      auto OptConstantValue = MDHelper::extractZExtI32Constant(MD->getOperand(2 * Idx + 1));
      if (!OptStatus.has_value() || !OptConstantValue.has_value())
        return make_error<StringError>("unexpected missing values", inconvertibleErrorCode());

      ArgSlotStatus Status = static_cast<ArgSlotStatus>(*OptStatus);
      if (Status >= ArgSlotStatus::Count)
        return make_error<StringError>("invalid status", inconvertibleErrorCode());
      Result.ArgSlots[Idx] = {Status, *OptConstantValue};
    }
    return Result;
  }

  llvm::MDNode *exportAsMetadata(LLVMContext &Context) const {
    SmallVector<Metadata *> Entries;
    unsigned NumEntries = 2 * ArgSlots.size();
    Entries.reserve(NumEntries);
    for (const ArgSlotInfo &ASI : ArgSlots) {
      // Serialize ArgSlotInfo using two 32-bit values: The first one gives
      // the status, the second one the constant (if there is one)
      Entries.push_back(MDHelper::getI32MDConstant(Context, static_cast<uint32_t>(ASI.Status)));
      Entries.push_back(MDHelper::getI32MDConstant(Context, ASI.ConstantValue));
    }
    return MDTuple::get(Context, Entries);
  }

  static ArgSlotsInfo combine(const ArgSlotsInfo &LHS, const ArgSlotsInfo &RHS) {
    ArgSlotsInfo Result;
    // Canonicalize which one is the larger one, this simplifies the combine logic
    const ArgSlotsInfo *SmallInfo = &LHS;
    const ArgSlotsInfo *LargeInfo = &RHS;
    if (SmallInfo->ArgSlots.size() > LargeInfo->ArgSlots.size())
      std::swap(SmallInfo, LargeInfo);

    Result.ArgSlots.reserve(LargeInfo->ArgSlots.size());

    for (unsigned ArgIdx = 0; ArgIdx < LargeInfo->ArgSlots.size(); ++ArgIdx) {
      if (ArgIdx < SmallInfo->ArgSlots.size())
        Result.ArgSlots.push_back(ArgSlotInfo::combine(SmallInfo->ArgSlots[ArgIdx], LargeInfo->ArgSlots[ArgIdx]));
      else
        Result.ArgSlots.push_back(LargeInfo->ArgSlots[ArgIdx]);
    }

    return Result;
  }

  void print(llvm::raw_ostream &OS, bool Compact = false) const {
    for (const auto &[Idx, ASI] : enumerate(ArgSlots)) {
      if (!Compact && Idx)
        OS << "; ";
      ASI.print(OS, Compact);
    }
  }

  // Prints a compact output, together with table headers indicating argument slot indices, like this:
  // <Indent>0         1         2
  // <Indent>012345678901234567890
  // <Indent>DDDDPCCDDDDDDPPDDDDDD
  void printTable(llvm::raw_ostream &OS, StringRef Indent) const {
    OS << Indent;
    if (ArgSlots.empty()) {
      OS << "<empty>\n";
      return;
    }
    for (unsigned Idx = 0; Idx < ArgSlots.size(); ++Idx) {
      if (Idx % 10 == 0)
        OS << (Idx / 10) % 10;
      else
        OS << ' ';
    }
    OS << '\n' << Indent;
    for (unsigned Idx = 0; Idx < ArgSlots.size(); ++Idx)
      OS << Idx % 10;
    OS << '\n' << Indent;
    print(OS, true);
    OS << '\n';
  }

  bool operator==(const ArgSlotsInfo &Other) const { return ArgSlots == Other.ArgSlots; }
  bool operator!=(const ArgSlotsInfo &Other) const { return !(*this == Other); }
};

[[maybe_unused]] llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ArgSlotsInfo &AI) {
  AI.print(OS);
  return OS;
}

// This is a simplified version ValueTracking::SliceInfo, specialized for the purpose of detecting
// preserved argument slot dwords. It stores a value it refers to, and a byte offset into that value.
//
// For every incoming argument slot, we create a DwordOriginInfo that points to the corresponding dword
// in the incoming argument.
// For every outgoing argument slot, we use value origin tracking to determine whether it in fact matches
// the corresponding incoming argument slot dword.
struct ValueWithOffset {
  Value *Val = nullptr;
  unsigned ByteOffset = -1;
  bool operator==(const ValueWithOffset &Other) const {
    return std::tie(Val, ByteOffset) == std::tie(Other.Val, Other.ByteOffset);
  }
  bool operator!=(const ValueWithOffset &Other) const { return !(*this == Other); }
};

struct IncomingArgSlotValuesWithOffsets {
  // Indexed by dword arg slot. For every incoming arg slot of a function, points into the scalar corresponding to
  // that argument slot within the argument containing the arg slot.
  // If an argument slot doesn't correspond to a full aligned dword within the containing argument type, then the value
  // of this arg slot is set to nullptr instead to indicate that we can't detect preservation of this arg slot.
  SmallVector<ValueWithOffset> ArgSlots;
  // For awaits during Traversal (e.g. ReportHit), we want to also allow preserving the awaited result instead of
  // incoming arguments.
  // We do this by telling the ValueOriginTracker to assume await results to equal corresponding incoming function args.
  // This is a mapping of awaited results to incoming arguments that can be passed to the value origin tracker
  // so it understands this assumption.
  // Use std::optional so we can safely move from this value and reset the optional, catching unintended accesses.
  std::optional<ValueOriginTracker::ValueOriginAssumptions> AwaitOriginAssumptions =
      ValueOriginTracker::ValueOriginAssumptions{};
};

// Info for a single arg slot as part of ArgumentLayoutInfo
class ArgumentLayoutSlotInfo {
public:
  ArgumentLayoutSlotInfo(unsigned ByteOffset, unsigned NumBytes)
      : ByteOffset{ByteOffset}, CoversAlignedDword{(ByteOffset % 4 == 0) && (NumBytes == 4)} {}
  // For the value corresponding to the arg slot within the containing type, stores the corresponding byte offset into
  // the as-in-memory layout of the type. For instance, given Ty = { i32, i64 }, and assuming i64 is 64-bit aligned,
  // then Ty occupies three arg slots at offsets 0, 8 and 12 into the type. The dword at offset 4 is padding and does
  // not have a corresponding arg slot.
  unsigned ByteOffset;
  // If the ByteOffset is not dword aligned, then we cannot keep track of this value with value tracking which uses
  // dword slices. Also, if the offset is dword aligned, but the value doesn't cover the whole dword, we as well
  // can't prove the value to be preserved, as we can't tell whether the whole value is preserved, or just a prefix.
  //
  // We currently handle small types that don't cover full dwords (e.g. i16) conservatively.
  // Some cases, e.g. just forwarding a single i16, are currently considered as dynamic where in fact
  // we could consider them as preserve, because only non-poison outgoing bits are relevant for the analysis.
  // However, other cases where incoming high implicit poison bits are populated may not be treated as preserve.
  // For instance, consider an incoming <2 x i16> %arg argument that covers two argument slots, but the type is a single
  // dword large. If the function bitcasts the argument to an i32 and passes that i32 to an outgoing argument slot,
  // value origin analysis on the i32 might conclude that it originates from a matching incoming argument slot
  // (value %arg, offset 0), and thus can be considered as preserve, missing the fact that the high 16 bits of the
  // argument slot were previously poison. These poison bits are not present in the <2 x i16> argument type.
  //
  // As long as we don't expect i16s in arguments, we thus keep the analysis simpler by handling i16s conservatively.
  bool CoversAlignedDword;
};

// Describes how a type is laid out in in-register argument slots.
class ArgumentLayoutInfo {
public:
  SmallVector<ArgumentLayoutSlotInfo> SlotInfos;

  unsigned numArgumentSlots() const { return SlotInfos.size(); }

  static ArgumentLayoutInfo get(Type *Ty, const DataLayout &DL) {
    ArgumentLayoutInfo Result{};
    populateRecursively(Ty, DL, Result, 0);
    return Result;
  }

private:
  // Recursively populate Result, assuming a (possibly nested) value of the given type at the given byte offset.
  static void populateRecursively(Type *Ty, const DataLayout &DL, ArgumentLayoutInfo &Result,
                                  unsigned AccumByteOffset) {
    // Detect how many arg slots we added, and at the end assert that it matches the expectation
    [[maybe_unused]] unsigned PrevNumArgSlots = Result.numArgumentSlots();
    if (auto *STy = dyn_cast<StructType>(Ty)) {
      const auto &SL = DL.getStructLayout(STy);
      for (unsigned ElemIdx = 0; ElemIdx < STy->getNumElements(); ++ElemIdx) {
        auto *ElemTy = STy->getElementType(ElemIdx);
        unsigned ByteOffset = SL->getElementOffset(ElemIdx);
        populateRecursively(ElemTy, DL, Result, AccumByteOffset + ByteOffset);
      }
    } else if (isa<VectorType>(Ty)) {
      // We don't support nor expect non-fixed vector types
      auto *VecTy = cast<FixedVectorType>(Ty);
      // Vectors are always bit-packed without padding.
      //
      // We support all vectors of element types with a byte-aligned size.
      // Element sizes do not have to be dword-aligned for this function to correctly
      // compute an argument layout info. However non-dword aligned elements might be handled
      // conservatively by the following analysis.
      //
      // We don't support vectors whose element types are not byte-aligned, as below code used byte-based offsets.
      // Such vectors should not be passed in arguments. If we realled need to support them in the future,
      // one possibility would be populating explicitly invalidated argument layout infos.
      Type *ElemTy = VecTy->getElementType();
      unsigned NumElemBits = DL.getTypeSizeInBits(ElemTy);
      assert(NumElemBits % 8 == 0);
      unsigned NumElemBytes = NumElemBits / 8;
      unsigned NumElemDwords = divideCeil(NumElemBytes, 4);
      unsigned NumElems = VecTy->getNumElements();
      for (unsigned ElemIdx = 0; ElemIdx < NumElems; ++ElemIdx) {
        unsigned NumRemainingBytes = NumElemBytes;
        for (unsigned DwordIdx = 0; DwordIdx < NumElemDwords; ++DwordIdx) {
          unsigned NumSlotBytes = std::min(4u, NumRemainingBytes);
          NumRemainingBytes -= NumSlotBytes;
          Result.SlotInfos.emplace_back(AccumByteOffset + 4 * DwordIdx, NumSlotBytes);
        }
        AccumByteOffset += NumElemBytes;
      }
    } else if (auto *ArrTy = dyn_cast<ArrayType>(Ty)) {
      Type *ElemTy = ArrTy->getElementType();
      unsigned NumElems = ArrTy->getNumElements();
      unsigned ElemStrideInBytes = DL.getTypeAllocSize(ElemTy).getFixedValue();
      for (unsigned ElemIdx = 0; ElemIdx < NumElems; ++ElemIdx)
        populateRecursively(ElemTy, DL, Result, AccumByteOffset + ElemIdx * ElemStrideInBytes);
    } else {
      assert(Ty->isSingleValueType());
      // Pointers, integers, floats
      unsigned NumBits = DL.getTypeSizeInBits(Ty);
      assert(NumBits % 8 == 0);
      unsigned NumBytes = NumBits / 8;
      unsigned NumDwords = divideCeil(NumBytes, 4);
      unsigned NumRemainingBytes = NumBytes;
      for (unsigned DwordIdx = 0; DwordIdx < NumDwords; ++DwordIdx) {
        unsigned NumSlotBytes = std::min(4u, NumRemainingBytes);
        NumRemainingBytes -= NumSlotBytes;
        Result.SlotInfos.emplace_back(AccumByteOffset + DwordIdx * 4, NumSlotBytes);
      }
    }
    [[maybe_unused]] unsigned NumAddedArgSlots = Result.numArgumentSlots() - PrevNumArgSlots;
    assert(NumAddedArgSlots == lgc::cps::getArgumentDwordCount(DL, Ty));
  }
};

// Stores an outgoing jump, together with the first outgoing argument that should be considered.
struct JumpInfo {
  CallInst *Outgoing = nullptr;
  unsigned FirstRelevantOutgoingArgIdx = 0;
};

struct AwaitInfo : public JumpInfo {
  // For awaits, we handle both lgc.cps.await and legacy awaits.
  // lgc.cps uses a single await call, like:
  //   %result = call @lgc.cps.await(i32 %target, i32 %levels, args...)
  // legacy mode uses *two* calls, first invoking target, and then awaiting the result:
  //   %handle = call ptr inttoptr (i32 %target to ptr)(args...)
  //   %result = call @await(ptr %handle)
  // For legacy awaits, this is the second call that obtains the result value.
  // For lgc.cps.await, it is the unique await call.
  CallInst *AwaitedResult = nullptr;
};

struct FunctionData {
  lgc::rt::RayTracingShaderStage Stage = lgc::rt::RayTracingShaderStage::Count;
  bool IsDuringTraversal = false;
  SmallVector<JumpInfo> Jumps;
  SmallVector<AwaitInfo> Awaits;
};

struct SpecializeDriverShadersPassImpl {
public:
  static constexpr unsigned ArgSlotSizeInBytes = 4;
  static constexpr unsigned MaxNumAnalyzedArgSlots = 256;

  Module &M;
  const DataLayout &DL;
  SpecializeDriverShadersOptions Opts;
  ArgSlotsInfo &TraversalArgsInfo;
  // If TraversalArgsInfo is trivial when starting the pass, meaning there was no metadata that
  // we could serialize from, conservatively do not optimize, because it could mean that
  // the pipeline compiler is not merging and propagating cross-module state.
  bool HadNonTrivialIncomingTraversalArgsInfo = true;
  MapVector<Function *, FunctionData> ToProcess;
  // We usually have only one, but supporting more is trivial and helps testing.
  SmallVector<Function *> TraversalFunctions;
  Type *I32 = nullptr;
  // When considering incoming function args to be preserved/specialized, ignore this many arguments.
  unsigned FirstRelevantIncomingArgIdx = -1;
  unsigned FirstRelevantOutgoingJumpArgIdx = -1;
  // Cache for per-type ArgumentLayoutInfos. unique_ptr for stable storage as DenseMap may invalidate iterators.
  DenseMap<Type *, std::unique_ptr<ArgumentLayoutInfo>> ArgLayoutInfos;

  SpecializeDriverShadersPassImpl(Module &M, ArgSlotsInfo &TraversalArgsInfo,
                                  const SpecializeDriverShadersOptions &Opts)
      : M{M}, DL{M.getDataLayout()}, Opts{Opts}, TraversalArgsInfo{TraversalArgsInfo}, I32{Type::getInt32Ty(
                                                                                           M.getContext())} {
    HadNonTrivialIncomingTraversalArgsInfo = !TraversalArgsInfo.ArgSlots.empty();
    if (ContHelper::isLgcCpsModule(M)) {
      // Ignore cont state, return addr, shaderRecIdx
      FirstRelevantIncomingArgIdx = 3;
      // Ignore: shaderAddr, levels, state, csp, returnAddr, shaderRecIdx
      FirstRelevantOutgoingJumpArgIdx = 6;
    } else {
      // Ignore returnAddr
      FirstRelevantIncomingArgIdx = 1;
      // Ignore: shaderAddr, levels, state, csp, returnAddr
      FirstRelevantOutgoingJumpArgIdx = 5;
    }
  }

  PreservedAnalyses run(ModuleAnalysisManager &AnalysisManager) {
    collectFunctions();
    collectJumpsAndAwaits();

    if (!Opts.DisableAnalysis) {
      for (auto &[F, Data] : ToProcess)
        analyze(F, Data);
    } else {
      LLVM_DEBUG({ dbgs() << "[SDS] Analysis is disabled, skipping"; });
    }

    bool DoSpecialize = true;
    if (TraversalFunctions.empty()) {
      DoSpecialize = false;
    } else if (!HadNonTrivialIncomingTraversalArgsInfo) {
      DoSpecialize = false;
      LLVM_DEBUG({ dbgs() << "[SDS] No incoming traversal args info, skipping specialization\n"; });
    } else if (Opts.DisableSpecialization) {
      DoSpecialize = false;
      LLVM_DEBUG({ dbgs() << "[SDS] Specialization disabled, skipping specialization\n"; });
    }
    if (DoSpecialize) {
      for (Function *TraversalFunc : TraversalFunctions)
        specializeFunction(TraversalFunc, TraversalArgsInfo);
      return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
  }

  void collectFunctions() {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      auto Stage = lgc::rt::getLgcRtShaderStage(&F);
      if (!Stage)
        continue;

      if (*Stage == lgc::rt::RayTracingShaderStage::Callable || *Stage == lgc::rt::RayTracingShaderStage::KernelEntry) {
        // CallShader is not allowed in AHS/Intersection, so we can ignore callable shaders.
        // Note that we don't have a way to differentiate TraceRay awaits from CallShader awaits
        // in RayGen/CHS/Miss, and so pessimistically include CallShader awaits in the analysis.
        continue;
      }

      FunctionData Data{};
      Data.Stage = *Stage;
      Data.IsDuringTraversal = [](lgc::rt::RayTracingShaderStage Stage) {
        switch (Stage) {
        case lgc::rt::RayTracingShaderStage::ClosestHit:
        case lgc::rt::RayTracingShaderStage::Miss:
        case lgc::rt::RayTracingShaderStage::RayGeneration: {
          return false;
        }
        case lgc::rt::RayTracingShaderStage::Intersection:
        case lgc::rt::RayTracingShaderStage::AnyHit:
          // For Traversal, we also analyze jumps out of Traversal to CHS/Miss, which is not required and could
          // restrict optimization opportunities unnecessarily. In practice, it shouldn't matter though.
        case lgc::rt::RayTracingShaderStage::Traversal:
          return true;
        case lgc::rt::RayTracingShaderStage::Callable:
        case lgc::rt::RayTracingShaderStage::KernelEntry:
        case lgc::rt::RayTracingShaderStage::Count:
          report_fatal_error("Unexpected shader stage " + Twine(static_cast<int>(Stage)));
        }
        report_fatal_error("Unknown shader stage " + Twine(static_cast<int>(Stage)));
      }(*Stage);

      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      if (*Stage == lgc::rt::RayTracingShaderStage::Traversal)
        TraversalFunctions.push_back(&F);
    }
  }

  void collectJumpsAndAwaits() {
    struct State {
      SpecializeDriverShadersPassImpl &Self;
    };

    static const auto HandleJumpOrAwait = [](State &State, Instruction &Op) {
      Function *F = Op.getFunction();
      auto *CI = cast<CallInst>(&Op);
      auto *It = State.Self.ToProcess.find(F);
      if (It == State.Self.ToProcess.end())
        return;

      FunctionData &Data = It->second;
      if (isa<lgc::cps::JumpOp>(Op)) {
        Data.Jumps.push_back({CI, State.Self.FirstRelevantOutgoingJumpArgIdx});
      } else {
        assert(isa<lgc::cps::AwaitOp>(Op));
        // ignore: shaderAddr, levels, shaderRecIdx
        Data.Awaits.push_back({{CI, 3}, CI});
      }
    };

    static const auto Visitor =
        llvm_dialects::VisitorBuilder<State>().addSet<lgc::cps::JumpOp, lgc::cps::AwaitOp>(HandleJumpOrAwait).build();

    State S{*this};
    Visitor.visit(S, M);

    // Also collect legacy awaits.
    // Because there can be multiple overloads, we need to collect all functions starting with "await".
    for (auto &F : M.functions()) {
      if (F.getName().starts_with("await")) {
        forEachCall(F, [&](CallInst &AwaitResult) {
          Function *ContainingFunc = AwaitResult.getFunction();
          auto *It = ToProcess.find(ContainingFunc);
          if (It == ToProcess.end())
            return; // ignore this call

          // Legacy awaits look like this:
          //   %awaitHandle = call ptr inttoptr (i32 %target to ptr)(args...)
          //   %awaitResult = call @await(ptr %awaitedResult)
          assert(AwaitResult.arg_size() == 1);
          auto *AwaitHandle = cast<CallInst>(AwaitResult.getArgOperand(0));
          assert(AwaitHandle->getType()->isPointerTy());
          FunctionData &Data = It->second;
          // Legacy awaited calls have only normal args.
          // The awaited function is indirectly called, and thus not an arg,
          // and the optional wait mask is on metadata.
          unsigned FirstRelevantArgIdx = 1; // ignore return address
          Data.Awaits.push_back({{AwaitHandle, FirstRelevantArgIdx}, &AwaitResult});
        });
      }
    }
  }

  const ArgumentLayoutInfo &getOrComputeArgumentLayoutInfo(Type *Ty) {
    auto It = ArgLayoutInfos.find(Ty);
    if (It == ArgLayoutInfos.end())
      It = ArgLayoutInfos.insert({Ty, std::make_unique<ArgumentLayoutInfo>(ArgumentLayoutInfo::get(Ty, DL))}).first;

    return *It->second;
  };

  // If IsDuringTraversal is false, returns a trivial info, because there is nothing to preserve.
  // Otherwise, collect incoming args, and a mapping of await results to incoming function args
  // so the value origin tracker handles await results like incoming function args.
  IncomingArgSlotValuesWithOffsets computeToBePreservedIncomingArgSlots(Function *F, const FunctionData &Data) {
    if (!Data.IsDuringTraversal)
      return {};

    IncomingArgSlotValuesWithOffsets Result{};

    // Collect incoming args
    for (unsigned ArgIdx = FirstRelevantIncomingArgIdx; ArgIdx < F->arg_size(); ++ArgIdx) {
      Value *Arg = F->getArg(ArgIdx);
      const ArgumentLayoutInfo &ArgLayoutInfo = getOrComputeArgumentLayoutInfo(Arg->getType());

      for (unsigned CurArgSlot = 0; CurArgSlot < ArgLayoutInfo.numArgumentSlots(); ++CurArgSlot) {
        auto LayoutSlotInfo = ArgLayoutInfo.SlotInfos[CurArgSlot];
        ValueWithOffset CurArgSlotInfo{Arg, 0};
        if (LayoutSlotInfo.CoversAlignedDword) {
          CurArgSlotInfo.ByteOffset = LayoutSlotInfo.ByteOffset;
        } else {
          // We can't analyze this arg slot. Invalidate.
          CurArgSlotInfo.Val = nullptr;
        }
        DETAIL_DEBUG({
          dbgs() << "[SDS] Analyze global incoming arg slot " << Result.ArgSlots.size() << ": ";
          if (CurArgSlotInfo.Val)
            dbgs() << *CurArgSlotInfo.Val << ", offset " << CurArgSlotInfo.ByteOffset << "\n";
          else
            dbgs() << "<unknown>\n";
        });
        Result.ArgSlots.push_back({CurArgSlotInfo});
      }
    }

    // Collect await results, decompose them into virtual incoming argument slots, and map
    // these argument slots to the corresponding incoming function argument slots.
    // Then, add assumptions for value origin tracking that assume await result argument
    // slots to equal the mapped incoming argument slots.
    // We could alternatively map them to the corresponding outgoing await args,
    // but that doesn't make a difference as the outgoing await is separately analyzed,
    // and non-preserved args are detected when doing that.
    Result.AwaitOriginAssumptions.emplace();
    for (const auto &AwaitInfo : Data.Awaits) {
      auto *AwaitResult = AwaitInfo.AwaitedResult;
      // Await results are expected to be a struct type that wraps the actual args
      // We treat the struct members like incoming function arguments,
      // because await lowering will turn the part after the await into a function that takes exactly
      // the struct members as arguments.
      // For each element of the struct, compute its argument layout, which gives a partial covering of the
      // in-memory-layout of the type with dwords as used in the argument layout.
      // Then, construct an OriginAssumption that maps those slices of the await result that
      // have a corresponding arg slot to the value and offset of that incoming arg slot,
      // and map slices covered by padding to themselves.
      // If there are argument slots that do not correspond to full aligned dword in the containing type,
      // conservatively ignore these arg slots, and to not add assumptions.
      auto *STy = cast<StructType>(AwaitResult->getType());
      assert(!STy->isPacked() && "packed await result structs not supported");
      const auto &SL = DL.getStructLayout(STy);
      ValueTracking::ValueInfo &OriginAssumption = (*Result.AwaitOriginAssumptions)[AwaitResult];

      unsigned AccumArgSlot = 0;
      bool Stop = false;
      for (unsigned ElemIdx = 0; ElemIdx < STy->getNumElements() && !Stop; ++ElemIdx) {
        auto *ElemTy = STy->getElementType(ElemIdx);
        unsigned ElementByteOffset = SL->getElementOffset(ElemIdx);
        if (ElementByteOffset % 4 != 0) {
          // Don't add assumptions for this element.
          continue;
        }
        const ArgumentLayoutInfo &ArgLayoutInfo = getOrComputeArgumentLayoutInfo(ElemTy);
        unsigned NumArgSlots = ArgLayoutInfo.numArgumentSlots();

        for (unsigned LocalArgSlotIdx = 0; LocalArgSlotIdx < NumArgSlots; ++LocalArgSlotIdx) {
          unsigned GlobalArgSlotIdx = AccumArgSlot + LocalArgSlotIdx;
          if (GlobalArgSlotIdx >= Result.ArgSlots.size()) {
            // We ran out of incoming arguments to map to, stop.
            // Assumptions on prefixes of values are supported.
            Stop = true;
            break;
          }
          // There is a corresponding incoming argument
          // Before we add this slice, mapping to the incoming arg slot,
          // ensure we are at the correct slice, and add dummy padding slices if necessary
          auto LayoutSlotInfo = ArgLayoutInfo.SlotInfos[LocalArgSlotIdx];
          if (!LayoutSlotInfo.CoversAlignedDword) {
            // Can't analyze this arg slot, don't add an assumption
            continue;
          }
          unsigned LocalByteOffset = LayoutSlotInfo.ByteOffset;
          while (OriginAssumption.Slices.size() * 4 < ElementByteOffset + LocalByteOffset) {
            ValueTracking::SliceInfo TrivialAssumption{ValueTracking::SliceStatus::Dynamic};
            TrivialAssumption.DynamicValue = AwaitResult;
            TrivialAssumption.DynamicValueByteOffset = OriginAssumption.Slices.size() * 4;
            OriginAssumption.Slices.push_back(TrivialAssumption);
          }
          assert(OriginAssumption.Slices.size() * 4 == ElementByteOffset + LocalByteOffset);
          const ValueWithOffset &InputArgSlotInfo = Result.ArgSlots[GlobalArgSlotIdx];
          if (InputArgSlotInfo.Val == nullptr) {
            // Overlapping scalars, can't analyze arg slot and can't add assumption
            continue;
          }
          ValueTracking::SliceInfo ArgSlotAssumption{ValueTracking::SliceStatus::Dynamic};
          ArgSlotAssumption.DynamicValue = InputArgSlotInfo.Val;
          ArgSlotAssumption.DynamicValueByteOffset = InputArgSlotInfo.ByteOffset;
          DETAIL_DEBUG({
            dbgs() << "[SDS] Mapping arg slot " << GlobalArgSlotIdx << " of await result ";
            AwaitResult->printAsOperand(dbgs());
            dbgs() << " (element idx " << ElemIdx << ", element type " << *ElemTy << ", local byte offset "
                   << LocalByteOffset << ") to input arg " << *InputArgSlotInfo.Val << ", offset "
                   << InputArgSlotInfo.ByteOffset << "\n";
          });
          OriginAssumption.Slices.push_back(ArgSlotAssumption);
        }
        AccumArgSlot += NumArgSlots;
      }
    }

    return Result;
  }

  // Given an outgoing arg slot and the value passed to it, determine the status of that arg slot (e.g. whether it
  // preserves an incoming one, passes a constant, an undef/poison, or an unknown dymamic value).
  // The arg slot is identified by GlobalArgSlotIndex.
  // For instance, the third arg slot in call(i32, i64 %foo) has the global arg slot index 2,
  // value %foo and local arg slot index 1, because it is the second dword of %foo.
  ArgSlotInfo computeOutgoingArgSlotInfo(const IncomingArgSlotValuesWithOffsets &ToBePreservedIncomingArgsInfos,
                                         Value *Arg, const ArgumentLayoutSlotInfo &LayoutSlotInfo,
                                         unsigned GlobalArgSlotIndex, ValueOriginTracker &VOT) {
    if (!LayoutSlotInfo.CoversAlignedDword) {
      DETAIL_DEBUG({ dbgs() << "[SDS] Can't analyze arg slot, doesn't cover aligned dword\n"; });
      return ArgSlotInfo{ArgSlotStatus::Dynamic};
    }
    unsigned LocalByteOffset = LayoutSlotInfo.ByteOffset;
    assert(LocalByteOffset % 4 == 0);
    const ValueTracking::ValueInfo &ArgVI = VOT.getValueInfo(Arg);
    DETAIL_DEBUG({
      dbgs() << "[SDS] byte offset " << LocalByteOffset << " of " << *Arg << ", global slot " << GlobalArgSlotIndex
             << "\n";
    });
    unsigned SliceIdx = LocalByteOffset / 4;

    if (SliceIdx >= ArgVI.Slices.size()) {
      // No value origin info for this arg slot, give up
      DETAIL_DEBUG(dbgs() << "[SDS] no slice info\n";);
      return ArgSlotInfo{ArgSlotStatus::Dynamic};
    }

    // we have a slice info for the current outgoing argument slot
    const ValueTracking::SliceInfo &ArgSI = ArgVI.Slices[SliceIdx];
    if (ArgSI.Status.contains(ValueTracking::SliceStatus::Dynamic)) {
      if (GlobalArgSlotIndex >= ToBePreservedIncomingArgsInfos.ArgSlots.size()) {
        // There is no corresponding incoming argument on the same slot, so we already know
        // this can't be a preserved value. Give up on this argument slot.
        DETAIL_DEBUG({ dbgs() << "[SDS] no incoming arg slot. SI: " << ArgSI << "\n"; });
        return ArgSlotInfo{ArgSlotStatus::Dynamic};
      }

      // In case the outgoing value is obtained from a phi node that forwards either an incoming
      // argument or an await result, the value origin assumptions that map await results
      // to input arguments allow the value origin tracker to point to input args in these cases,
      // also with nested phis. Thus, we don't have to deal with phi nodes here,
      // and can directly compare against the incoming arg.
      if (ToBePreservedIncomingArgsInfos.ArgSlots[GlobalArgSlotIndex] !=
          ValueWithOffset{ArgSI.DynamicValue, ArgSI.DynamicValueByteOffset}) {
        DETAIL_DEBUG({
          const auto &TBP = ToBePreservedIncomingArgsInfos.ArgSlots[GlobalArgSlotIndex];
          dbgs() << "[SDS] no match. ArgSI: " << ArgSI << ", to be preserved: " << *TBP.Val << ", offset "
                 << TBP.ByteOffset << "\n";
        });
        return ArgSlotInfo{ArgSlotStatus::Dynamic};
      }

      // All paths that use a dynamic value for this outgoing arg slot preserve the incoming arg slot,
      // so we can ignore this. Check other status first, and assign Preserve status if there are no others.
    }

    if (ArgSI.Status.contains(ValueTracking::SliceStatus::Constant)) {
      // Do this even if the value might be undef, as it is feasible to combine undef and constant into constant.
      // If we want to conservatively treat undef/poison as zero in the future, we'd need to change this.
      DETAIL_DEBUG({ dbgs() << "[SDS] Constant: " << ArgSI.ConstantValue << "\n"; });
      return ArgSlotInfo{ArgSlotStatus::Constant, ArgSI.ConstantValue};
    }

    if (ArgSI.Status.contains(ValueTracking::SliceStatus::UndefOrPoison)) {
      DETAIL_DEBUG({ dbgs() << "[SDS] UndefOrPoison:\n"; });
      return ArgSlotInfo{ArgSlotStatus::UndefOrPoison};
    }

    assert(ArgSI.Status == ValueTracking::SliceStatus::Dynamic);
    DETAIL_DEBUG({ dbgs() << "[SDS] Preserve:\n"; });
    return ArgSlotInfo{ArgSlotStatus::Preserve};
  }

#ifndef NDEBUG
  // Sort JumpInfos by instruction order in the containing function.
  // This ensures processing order (and thereby debug output order) matches input IR order for lit tests.
  void sortByInstructionOrder(SmallVectorImpl<JumpInfo> &JumpInfos) const {
    if (JumpInfos.empty())
      return;
    Function *F = JumpInfos[0].Outgoing->getFunction();

    // Maps instructions to entry indices in JumpInfos
    SmallDenseMap<const Instruction *, unsigned> JumpToIndex;
    for (const auto &[Index, JumpInfo] : enumerate(JumpInfos)) {
      assert(JumpInfo.Outgoing->getFunction() == F);
      [[maybe_unused]] auto Inserted = JumpToIndex.insert({JumpInfo.Outgoing, Index}).second;
      assert(Inserted);
    }

    SmallVector<JumpInfo> Result;
    Result.reserve(JumpInfos.size());
    for (const auto &BB : *F) {
      for (const auto &Inst : BB) {
        auto It = JumpToIndex.find(&Inst);
        if (It != JumpToIndex.end()) {
          Result.push_back(JumpInfos[It->second]);
          JumpToIndex.erase(It);
        }
      }
    }
    assert(Result.size() == JumpInfos.size());

    JumpInfos = std::move(Result);
  }
#endif

  // Collect and return the set of outgoing jumps/awaits that may be during Traversal.
  SmallVector<JumpInfo> getRelevantOutgoingJumpsAndAwaits(const FunctionData &Data) const {
    SmallVector<JumpInfo> JumpsAndAwaits;
    JumpsAndAwaits.reserve(Data.Jumps.size() + Data.Awaits.size());
    for (const auto &AwaitInfo : Data.Awaits)
      JumpsAndAwaits.push_back(AwaitInfo);

    // Ignore jumps in shaders outside of Traversal:
    // These are shader returns, and thus are neither during Traversal, nor entering Traversal.
    if (Data.IsDuringTraversal)
      JumpsAndAwaits.append(Data.Jumps);

#ifndef NDEBUG
    if (M.getNamedMetadata("lgc.rt.specialize.driver.shaders.process.in.instruction.order"))
      sortByInstructionOrder(JumpsAndAwaits);
#endif

    return JumpsAndAwaits;
  }

  // This is a performance optimization.
  // We know that we are going to query the ValueOriginTracker about all arguments passed to all of these
  // jumps and awaits. The value origin analysis is more efficient when done in bulk, so do that here.
  // The later queries will then return cached results.
  void runValueTrackingAnalysisOnAllOutgoingArgs(ValueOriginTracker &VOT, ArrayRef<JumpInfo> JumpsAndAwaits) {
    SmallVector<Value *> OutgoingArgs;
    for (const auto &JumpOrAwait : JumpsAndAwaits) {
      for (unsigned OutgoingArgIdx = JumpOrAwait.FirstRelevantOutgoingArgIdx;
           OutgoingArgIdx < JumpOrAwait.Outgoing->arg_size(); ++OutgoingArgIdx) {
        Value *OutgoingArg = JumpOrAwait.Outgoing->getArgOperand(OutgoingArgIdx);
        // This might add duplicates, but that's fine.
        OutgoingArgs.push_back(OutgoingArg);
      }
    }
    VOT.analyzeValues(OutgoingArgs);
  }

  void analyze(Function *F, FunctionData &Data) {
    // We analyze both jumps and awaits.
    // We treat all awaits as potentially starting or continuing Traversal.
    // This is accurate for TraceRay and ReportHit, and pessimistic for CallShader.
    //
    // At this stage, before coro passes, jumps come from two sources:
    //   * app shader returns
    //   * Traversal enqueues
    //
    // In both cases, we determine based on the shader type whether jumps may be in Traversal state.
    // For in-Traversal shaders, we analyze all jumps and awaits, and preserving arguments is allowed.
    // Otherwise (CHS/Miss/RGS), we ignore outgoing jumps, as they come from app shader returns outside
    // of Traversal, and do not allow preserving arguments in awaits, because the incoming arguments of these
    // shaders are set up outside of the Traversal state.

    // Collect information about incoming arguments and results returned by awaits.
    // These are used to determine potential preserved arguments.
    auto ToBePreservedInputArgsInfo = computeToBePreservedIncomingArgSlots(F, Data);

    // Filter relevant jumps and awaits. Ignore those known to happen outside of Traversal.
    auto JumpsAndAwaits = getRelevantOutgoingJumpsAndAwaits(Data);

    // Initialize a new value origin tracker for the current function.
    // Move AwaitOriginAssumptions into the VOT to prevent a copy, and reset the optional
    // to prevent unintended accesses.
    CompilerUtils::ValueOriginTracker::Options Opts{};
    Opts.BytesPerSlice = ArgSlotSizeInBytes;
    Opts.MaxBytesPerValue = MaxNumAnalyzedArgSlots * ArgSlotSizeInBytes;
    // Handle freeze poison conservatively. Optimizing based on it requires to replace affected freeze poison
    // by something else (e.g. zeroinitializer), which means we'd need to change app shaders and not just
    // Traversal. As of now, in tests it didn't make a difference.
    Opts.FreezeMode = CompilerUtils::ValueOriginTracker::Options::FreezeHandlingMode::Dynamic;
    CompilerUtils::ValueOriginTracker VOT{DL, Opts, std::move(*ToBePreservedInputArgsInfo.AwaitOriginAssumptions)};
    ToBePreservedInputArgsInfo.AwaitOriginAssumptions.reset();

    // Do a bulk value origin analysis on all relevant outgoing args. This is more efficient than individual
    // queries.
    runValueTrackingAnalysisOnAllOutgoingArgs(VOT, JumpsAndAwaits);

    LLVM_DEBUG(dbgs() << "[SDS] Analyzing function " << F->getName() << " (shader stage " << Data.Stage << ")\n";);

    // The summary of preserved/constant outgoing argument infos for this function
    ArgSlotsInfo FuncArgsInfo;
    for (auto [JumpOrAwait, FirstRelevantArgIdx] : JumpsAndAwaits) {
      // The different jump or continue intrinsics have a different amount of "system" arguments that are not
      // actually passed as argument to the jumped-to function, e.g. the function itself, or possibly a wait mask.
      // These system arguments come before the actual arguments, and need to be ignored for the argument
      // analysis.

      ArgSlotsInfo CurOutgoingArgsInfo{};
      unsigned AccumulatedArgSlotIndex = 0;

      for (unsigned ArgIdx = FirstRelevantArgIdx; ArgIdx < JumpOrAwait->arg_size(); ++ArgIdx) {
        Value *Arg = JumpOrAwait->getArgOperand(ArgIdx);
        Type *ArgTy = Arg->getType();
        const ArgumentLayoutInfo &ArgLayoutInfo = getOrComputeArgumentLayoutInfo(ArgTy);
        unsigned NumArgSlots = ArgLayoutInfo.numArgumentSlots();

        // LocalArgSlot indexes into arg slots used by the current argument
        for (unsigned LocalArgSlotIndex = 0; LocalArgSlotIndex < NumArgSlots; ++LocalArgSlotIndex) {
          // GlobalArgSlot indexes into all arg slots
          unsigned GlobalArgSlotIndex = AccumulatedArgSlotIndex + LocalArgSlotIndex;
          const auto &LayoutSlotInfo = ArgLayoutInfo.SlotInfos[LocalArgSlotIndex];
          CurOutgoingArgsInfo.ArgSlots.push_back(
              computeOutgoingArgSlotInfo(ToBePreservedInputArgsInfo, Arg, LayoutSlotInfo, GlobalArgSlotIndex, VOT));
        }
        AccumulatedArgSlotIndex += NumArgSlots;
      }
      LLVM_DEBUG({
        dbgs() << "[SDS] Analyzed outgoing call " << *JumpOrAwait << "\n";
        CurOutgoingArgsInfo.printTable(dbgs(), "[SDS] ");
      });
      FuncArgsInfo = ArgSlotsInfo::combine(FuncArgsInfo, CurOutgoingArgsInfo);
    }

    LLVM_DEBUG({
      dbgs() << "[SDS] Finished analysis of function " << F->getName() << "\n";
      FuncArgsInfo.printTable(dbgs(), "[SDS] ");
    });
    TraversalArgsInfo = ArgSlotsInfo::combine(TraversalArgsInfo, FuncArgsInfo);
  }

  // GlobalArgSlotBegin is the index of the first argument slot occupied by this argument.
  struct SpecializeArgResult {
    Value *Replacement;
    unsigned NumToBeReplacedDwords;
    unsigned NumReplacedDwords;
  };

  using ValueSpecializer = CompilerUtils::ValueSpecializer;

  SpecializeArgResult specializeArgument(const ArgSlotsInfo &SpecializationInfo, ValueSpecializer &VS, Argument *Arg,
                                         const ArgumentLayoutInfo &ArgumentLayoutInfo, unsigned GlobalArgSlotBegin) {
    unsigned NumArgSlots = ArgumentLayoutInfo.numArgumentSlots();
    // Set up data for ValueSpecializer. This requires converting the specialization info from per-arg-slot to
    // per-dword.
    SmallVector<ValueSpecializer::DwordSpecializationInfo> SpecializationInfos;
    unsigned NumBytes = DL.getTypeStoreSize(Arg->getType());
    unsigned NumDwords = divideCeil(NumBytes, 4);
    SpecializationInfos.reserve(NumDwords);
    unsigned NumToBeReplacedDwords = 0;

    for (unsigned LocalArgSlotIdx = 0; LocalArgSlotIdx < NumArgSlots; ++LocalArgSlotIdx) {
      unsigned GlobalArgSlotIdx = GlobalArgSlotBegin + LocalArgSlotIdx;
      if (GlobalArgSlotIdx >= SpecializationInfo.ArgSlots.size()) {
        // No info about this incoming arg slot or further ones, fill up with dynamic fallback ones at the end.
        break;
      }
      const auto &ArgSlotInfo = SpecializationInfo.ArgSlots[GlobalArgSlotIdx];
      if (ArgSlotInfo.Status == ArgSlotStatus::Dynamic) {
        // Can't specialize dynamic arg slot
        continue;
      }
      LLVM_DEBUG(
          { dbgs() << "[SDS] Trying to specialize arg slot " << GlobalArgSlotIdx << " for " << ArgSlotInfo << "\n"; });

      const auto &LayoutSlotInfo = ArgumentLayoutInfo.SlotInfos[LocalArgSlotIdx];
      if (!LayoutSlotInfo.CoversAlignedDword) {
        LLVM_DEBUG(
            { dbgs() << "[SDS] Can't analyze arg slot " << GlobalArgSlotIdx << ", doesn't cover aligned dword\n"; });
        continue;
      }

      unsigned LocalByteOffset = LayoutSlotInfo.ByteOffset;
      assert(LocalByteOffset % 4 == 0);
      unsigned LocalDwordOffset = LocalByteOffset / 4;

      while (SpecializationInfos.size() < LocalDwordOffset)
        SpecializationInfos.push_back({ValueSpecializer::SpecializationKind::None});
      assert(SpecializationInfos.size() == LocalDwordOffset);

      ValueSpecializer::DwordSpecializationInfo SpecializationInfo{};
      if (ArgSlotInfo.Status == ArgSlotStatus::Constant) {
        SpecializationInfo.Kind = ValueSpecializer::SpecializationKind::Constant;
        SpecializationInfo.ConstantValue = ArgSlotInfo.ConstantValue;
      } else {
        assert(ArgSlotInfo.Status == ArgSlotStatus::UndefOrPoison || ArgSlotInfo.Status == ArgSlotStatus::Preserve);
        // If an argument slot is preserved by all shaders, and isn't constant or dynamic,
        // then it is never initialized, and can be assumed to be poison.
        // Use frozen poison to prevent propagation of poison into the containing value.
        SpecializationInfo.Kind = ValueSpecializer::SpecializationKind::FrozenPoison;
      }
      SpecializationInfos.push_back(SpecializationInfo);
      ++NumToBeReplacedDwords;
    }

    while (SpecializationInfos.size() < NumDwords)
      SpecializationInfos.push_back({ValueSpecializer::SpecializationKind::None});

    if (NumToBeReplacedDwords == 0) {
      // Nothing to be done
      return {};
    }

    // Preserve the builder insertion point, so argument specialization code is in argument order.
    // This improves test readability.
    auto [Replacement, NumReplacedDwords] =
        VS.replaceDwords(Arg, SpecializationInfos, /* replace uses */ true, /* preserve insert point */ true);
    return {Replacement, NumToBeReplacedDwords, NumReplacedDwords};
  }

  void specializeFunction(Function *Func, const ArgSlotsInfo &SpecializationInfo) {
    LLVM_DEBUG({
      dbgs() << "[SDS] Specializing function, final args info:\n";
      TraversalArgsInfo.printTable(dbgs(), "[SDS] ");
    });
    unsigned TotalNumToBeReplacedDwords = 0;
    unsigned TotalNumReplacedDwords = 0;
    unsigned AccumArgSlotIdx = 0;
    ValueSpecializer VS{*Func->getParent()};

    for (unsigned ArgIdx = FirstRelevantIncomingArgIdx; ArgIdx < Func->arg_size(); ++ArgIdx) {
      Argument *Arg = Func->getArg(ArgIdx);
      const auto &ArgumentLayoutInfo = getOrComputeArgumentLayoutInfo(Arg->getType());
      auto Result = specializeArgument(SpecializationInfo, VS, Arg, ArgumentLayoutInfo, AccumArgSlotIdx);
      TotalNumToBeReplacedDwords += Result.NumToBeReplacedDwords;
      TotalNumReplacedDwords += Result.NumReplacedDwords;
      AccumArgSlotIdx += ArgumentLayoutInfo.numArgumentSlots();
      if (AccumArgSlotIdx >= TraversalArgsInfo.ArgSlots.size())
        break;
    }
    LLVM_DEBUG({
      dbgs() << "[SDS] Replaced " << TotalNumReplacedDwords << " dwords in total, tried " << TotalNumToBeReplacedDwords
             << " dwords.\n";
    });
  }
};

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// SpecializeDriverShadersState::Impl

// Pimpl implementation class for SpecializeDriverShadersState.
struct SpecializeDriverShadersState::Impl {
  using Self = SpecializeDriverShadersState::Impl;

  ArgSlotsInfo TraversalArgsInfo;

  static llvm::Expected<Self> decodeMsgpack(llvm::msgpack::DocNode &Node) {
    auto &MapNode = Node.getMap();

    uint64_t Version = 0;
    auto VersionNode = MapNode[MsgPackFormat::Version];
    if (!VersionNode.isEmpty())
      Version = VersionNode.getUInt();
    if (Version != MsgPackFormat::MajorVersion)
      return make_error<StringError>("bad/missing specialize-driver-shaders version", inconvertibleErrorCode());

    Self Result{};

    auto &TraversalNode = MapNode[MsgPackFormat::TraversalArgsInfo];
    auto TraversalArgsInfoOrErr = ArgSlotsInfo::decodeMsgpack(TraversalNode);
    if (auto Err = TraversalArgsInfoOrErr.takeError())
      return Err;

    Result.TraversalArgsInfo = *TraversalArgsInfoOrErr;
    return Result;
  }

  void encodeMsgpack(llvm::msgpack::DocNode &Node) const {
    auto &MapNode = Node.getMap(true);
    MapNode[MsgPackFormat::Version] = MsgPackFormat::MajorVersion;
    auto &TraversalNode = MapNode[MsgPackFormat::TraversalArgsInfo];
    TraversalArgsInfo.encodeMsgpack(TraversalNode);
  }

  static llvm::Expected<Self> fromModuleMetadata(const llvm::Module &M) {
    auto *MD = M.getNamedMetadata(MetadataFormat::State);
    if (!MD) {
      // If there is no metadata, start with a trivial state.
      return Self{};
    }
    unsigned NumOperands = MD->getNumOperands();
    if (NumOperands != 1)
      return make_error<StringError>("unexpected number of nodes", inconvertibleErrorCode());

    Self Result{};
    auto AIOrErr = ArgSlotsInfo::fromMetadata(MD->getOperand(0));
    if (auto Err = AIOrErr.takeError())
      return Err;
    Result.TraversalArgsInfo = *AIOrErr;
    LLVM_DEBUG(Result.TraversalArgsInfo.printTable(dbgs(), "[SDS] Deserialized state from MD: "););

    return Result;
  }

  void exportModuleMetadata(llvm::Module &M) const {
    auto *MD = M.getOrInsertNamedMetadata(MetadataFormat::State);
    MD->clearOperands();
    MD->addOperand(TraversalArgsInfo.exportAsMetadata(M.getContext()));
    LLVM_DEBUG(TraversalArgsInfo.printTable(dbgs(), "[SDS] Serialized state to MD: "););
  }

  void merge(const Self &Other) {
    TraversalArgsInfo = ArgSlotsInfo::combine(TraversalArgsInfo, Other.TraversalArgsInfo);
  }

  bool operator==(const Impl &Other) const { return TraversalArgsInfo == Other.TraversalArgsInfo; }
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// SpecializeDriverShadersOptions
llvm::Expected<SpecializeDriverShadersOptions>
SpecializeDriverShadersOptions::fromModuleMetadata(const llvm::Module &M) {
  auto *MD = M.getNamedMetadata(MetadataFormat::Options);
  if (!MD) {
    // If there is no metadata, start with trivial options.
    return SpecializeDriverShadersOptions{};
  }

  unsigned NumOperands = MD->getNumOperands();
  if (NumOperands != 1)
    return make_error<StringError>("unexpected number of nodes", inconvertibleErrorCode());

  auto *OptsNode = MD->getOperand(0);

  auto OptDisableSpecialization = MDHelper::extractZExtI32Constant(OptsNode->getOperand(0));
  auto OptDisableAnalysis = MDHelper::extractZExtI32Constant(OptsNode->getOperand(1));

  if (!OptDisableSpecialization.has_value() || !OptDisableAnalysis.has_value())
    return make_error<StringError>("failed to import numeric options", inconvertibleErrorCode());

  if (OptDisableSpecialization.value() >= 2u || OptDisableAnalysis >= 2u)
    return make_error<StringError>("invalid numerical boolean values", inconvertibleErrorCode());

  SpecializeDriverShadersOptions Result{};
  Result.DisableAnalysis = (*OptDisableAnalysis != 0);
  Result.DisableSpecialization = (*OptDisableSpecialization != 0);
  return Result;
}

void SpecializeDriverShadersOptions::exportModuleMetadata(llvm::Module &M) const {
  auto *MD = M.getOrInsertNamedMetadata(MetadataFormat::Options);
  MD->clearOperands();
  MD->addOperand(MDTuple::get(M.getContext(), {MDHelper::getI32MDConstant(M.getContext(), DisableSpecialization),
                                               MDHelper::getI32MDConstant(M.getContext(), DisableAnalysis)}));
  // In debug builds, after serializing, check that deserializing yields the expected result
  assert(cantFail(fromModuleMetadata(M)) == *this);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// SpecializeDriverShadersState
SpecializeDriverShadersState::SpecializeDriverShadersState() : Pimpl{std::make_unique<Impl>()} {
}

SpecializeDriverShadersState::SpecializeDriverShadersState(const SpecializeDriverShadersState &Other)
    : SpecializeDriverShadersState() {
  if (Other.Pimpl)
    *Pimpl = *Other.Pimpl;
}

SpecializeDriverShadersState::SpecializeDriverShadersState(SpecializeDriverShadersState &&Other)
    : Pimpl(std::move(Other.Pimpl)) {
}

SpecializeDriverShadersState::SpecializeDriverShadersState(std::unique_ptr<Impl> Pimpl) : Pimpl(std::move(Pimpl)) {
}

SpecializeDriverShadersState::~SpecializeDriverShadersState() noexcept = default;

SpecializeDriverShadersState &SpecializeDriverShadersState::operator=(const SpecializeDriverShadersState &Other) {
  if (!Other.Pimpl) {
    Pimpl.reset();
  } else {
    if (Pimpl)
      *Pimpl = *Other.Pimpl;
    else
      Pimpl = std::make_unique<Impl>(*Other.Pimpl);
  }
  return *this;
}

SpecializeDriverShadersState &SpecializeDriverShadersState::operator=(SpecializeDriverShadersState &&Other) {
  Pimpl = std::move(Other.Pimpl);
  return *this;
}

llvm::Expected<SpecializeDriverShadersState> SpecializeDriverShadersState::decodeMsgpack(llvm::msgpack::DocNode &Node) {
  auto Result = Impl::decodeMsgpack(Node);
  if (auto Err = Result.takeError())
    return Err;
  return Self{std::make_unique<Impl>(*Result)};
}

void SpecializeDriverShadersState::encodeMsgpack(llvm::msgpack::DocNode &Node) const {
  assert(Pimpl && "Using invalid moved-from object");
  Pimpl->encodeMsgpack(Node);
  // In debug builds, after serializing, check that deserializing yields the expected result
  assert(cantFail(Impl::decodeMsgpack(Node)) == *Pimpl);
}

llvm::Expected<SpecializeDriverShadersState> SpecializeDriverShadersState::fromModuleMetadata(const llvm::Module &M) {
  auto Result = Impl::fromModuleMetadata(M);
  if (auto Err = Result.takeError())
    return Err;
  return Self{std::make_unique<Impl>(*Result)};
}

void SpecializeDriverShadersState::exportModuleMetadata(llvm::Module &M) const {
  assert(Pimpl && "Using invalid moved-from object");
  Pimpl->exportModuleMetadata(M);
  // In debug builds, after serializing, check that deserializing yields the expected result
  assert(cantFail(Impl::fromModuleMetadata(M)) == *Pimpl);
}

void SpecializeDriverShadersState::merge(SpecializeDriverShadersState const &Other) {
  assert(Pimpl && Other.Pimpl && "Using invalid moved-from object");
  Pimpl->merge(*Other.Pimpl);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// SpecializeDriverShadersPass
llvm::PreservedAnalyses SpecializeDriverShadersPass::run(llvm::Module &Module,
                                                         llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the specialize-driver-shaders pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  auto StateOrErr = SpecializeDriverShadersState::Impl::fromModuleMetadata(Module);
  if (!StateOrErr)
    report_fatal_error(StateOrErr.takeError());
  SpecializeDriverShadersState::Impl State = std::move(*StateOrErr);

  auto OptsOrErr = SpecializeDriverShadersOptions::fromModuleMetadata(Module);
  if (!OptsOrErr)
    report_fatal_error(OptsOrErr.takeError());
  SpecializeDriverShadersOptions Opts = *OptsOrErr;

  auto Result = SpecializeDriverShadersPassImpl{Module, State.TraversalArgsInfo, Opts}.run(AnalysisManager);

  State.exportModuleMetadata(Module);
  return Result;
}
