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
/**
 ***********************************************************************************************************************
 * @file  ValueOriginTracking.h
 * @brief Helpers for tracking the byte-wise origin of SSA values.
 *
 * @details
 * Sometimes we are interested in the byte-wise contents of a value.
 * If the value is a constant, this can be determined with standard LLVM helpers like computeKnownBits,
 * but even if the value is dynamic it can be helpful to trace where these bytes come from.
 *
 * For instance, if some outgoing function arguments de-facto preserve incoming function arguments in the same argument
 * slot, then this information may be used to enable certain inter-procedural optimizations.
 *
 * This file provides helpers for such an analysis.
 * It can be thought of splitting values into "slices" (e.g. bytes or dwords), and performing an analysis of where
 * these values come from, propagating through things like {insert,extract}{value,element}.
 * Using single-byte slices results in a potentially more accurate analysis, but has higher runtime cost.
 * For every value, the analysis works on the in-memory layout of its type, including padding, even though we analyze
 * only SSA values that might end up in registers.
 * It can be thought of as describing the memory obtained from storing a value to memory.
 *
 * In that sense, it is similar to how SROA splits up allocas into ranges, and analyses ranges separately.
 * However, we only track contents of SSA values, and do not propagate through memory, and thus generally
 * SROA should have been run before to eliminate non-necessary memory operations.
 *
 * If the client code has extra information on the origin of some intermediate values that this analysis cannot reason
 * about, e.g. calls to special functions, or special loads, then it can provide this information in terms of
 * assumptions, which use the same format as the analysis result, mapping slices of a value to slices of other values or
 * constants. When analyzing a value with an assumption on it, the algorithm then applies the analysis result for
 * values referenced by assumptions, and propagates the result through following instructions.
 *
 * The analysis does not modify functions, however, as part of the analysis, additional constants may be created.
 *
 * The motivating application that we have implemented this for is propagating constant known arguments into the
 * Traversal shader in continuations-based ray tracing:
 *
 * The Traversal shader is enqueued by potentially multiple call sites in RayGen (RGS), Closest-Hit (CHS) or Miss (MS)
 * shaders. If all these call sites share some common constant arguments (e.g. on the ray payload), then we may
 * want to propagate these constants into the Traversal shader to reduce register pressure.
 * On these call sites, a simple analysis based on known constant values suffices.
 *
 * However, the Traversal shader is re-entrant, and may enqueue itself. Also, with Any-Hit (AHS) and/or Intersection
 * (IS) shaders in the pipeline, these shaders are enqueued by Traversal, which in turn re-enqueue Traversal.
 *
 * Thus, in order to prove that incoming arguments of the Traversal shader are known constants, we need to prove
 * that all TraceRay call sites share these constants, *and* that all functions that might re-enqueue Traversal
 * (Traversal itself, AHS, IS) preserve these arguments, or set it to the same constant.
 *
 * This analysis allows all of the above: It allows to prove that certain outgoing arguments at TraceRay call sites
 * have a specific constant value, and allow to prove that outgoing arguments of Traversal/AHS/IS preserve the
 * corresponding incoming ones, or more precisely, that argument slots are preserved.
 * Because we track on a fine granularity (e.g. dwords), we might be able to prove that parts of a struct argument are
 * preserved even if some fields of it are changed.
 *
 ***********************************************************************************************************************
 */

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

namespace llvm {
class raw_ostream;
class Constant;
class DataLayout;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace CompilerUtils {

namespace ValueTracking {

// enum wrapper with some convenience helpers for common operations.
// The contained value is a bitmask of status, and thus multiple status can be set.
// In that case we know that at run time, one of the status holds, but we don't know which one.
// This can occur with phi nodes and select instructions.
// In the common cases, just a single bit is set though.
struct SliceStatus {
  // As the actual enum is contained within the struct, its values don't leak into the containing namespace,
  // and it's not possible to implicitly cast a SliceStatus to an int, so it's as good as an enum class.
  // The UndefOrPoison case always originates from a `poison` or `undef` value.
  // We must be careful with freeze instructions operating on such values, see FreezeHandlingMode.
  enum StatusEnum : uint32_t { Constant = 0x1, Dynamic = 0x2, UndefOrPoison = 0x4 };
  StatusEnum S = {};

  // Intentionally allow implicit conversion:
  SliceStatus(StatusEnum S) : S{S} {}

  static SliceStatus makeEmpty() { return static_cast<StatusEnum>(0); }

  // Returns whether all status bits set in other are also set in us.
  bool contains(SliceStatus Other) const { return (*this & Other) == Other; }

  // Returns whether no status bits are set.
  bool isEmpty() const { return static_cast<uint32_t>(S) == 0; }

  // Returns whether there is exactly one status bit set. Returns false for an empty status.
  bool isSingleStatus() const {
    auto AsInt = static_cast<uint32_t>(S);
    return (AsInt != 0) && (((AsInt - 1) & AsInt) == 0);
  }

  SliceStatus operator&(SliceStatus Other) const { return static_cast<StatusEnum>(S & Other.S); }

  SliceStatus operator|(SliceStatus Other) const { return static_cast<StatusEnum>(S | Other.S); }

  bool operator==(SliceStatus Other) const { return S == Other.S; }
  bool operator!=(SliceStatus Other) const { return !(S == Other.S); }
};

static constexpr unsigned MaxSliceSize = 4; // Needed for SliceInfo::ConstantValue

// A slice consists of a consecutive sequence of bytes within the representation of a value.
// We keep track of a potential constant value, and a potential dynamic value that determines
// the byte representation of our slice.
// If both dynamic and constant values are set, then one of them determines the byte representation
// of our slice, but we don't know which.
// If just a single value is set, then we know that that one determines us.
//
// Allowing both a dynamic and a constant value is intended to allow patterns where a value
// is either a constant, or a passed-through argument. If the constant matches the values used
// to initialize the incoming argument on the caller side, then we can still prove that the value
// is in fact constant.
//
// If the bit width of a value is not a multiple of the slice size, the last slice contains
// unspecified high bits. These are not guaranteed to be zeroed out.
struct SliceInfo {
  SliceInfo(SliceStatus S) : Status{S} {}
  void print(llvm::raw_ostream &OS, bool Compact = false) const;

  // Enum-bitmask of possible status of the value.
  SliceStatus Status = SliceStatus::makeEmpty();
  uint32_t ConstantValue = 0;
  static_assert(sizeof(ConstantValue) >= MaxSliceSize);
  // If set, the byte representation of this slice is obtained
  // from the given value at the given offset.
  llvm::Value *DynamicValue = nullptr;
  unsigned DynamicValueByteOffset = 0;
};
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const SliceInfo &BI);

// Combines slice infos for a whole value, unless the value is too large, in which case it might be cut off.
// It is up to client code to detect missing slice infos at the value tail if that is relevant,
// e.g. in order to prove that all bytes in a value match some assumption.
struct ValueInfo {
  void print(llvm::raw_ostream &OS, bool Compact = false) const;

  // Infos for the byte-wise representation of a value, partitioned into consecutive slices
  llvm::SmallVector<SliceInfo> Slices;
};
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ValueInfo &VI);

} // namespace ValueTracking

// Utility class to track the origin of values, partitioned into slices of e.g. 1 or 4 bytes each.
// See the documentation at the top of this file for details.
//
// The status of each slice is given by its SliceStatus.
// If the size of a value exceeds MaxBytesPerValue, then only a prefix of that size is analyzed.
// This ensures bounded runtime and memory consumption on pathological cases with huge values.
//
// This is intended to be used for interprocedural optimizations, detecting cases where arguments are initialized with a
// constant and then always propagated, allowing to replace the argument by the initial constant.
class ValueOriginTracker {
public:
  // Configuration options for ValueOriginTracker.
  struct Options {
    unsigned BytesPerSlice = 4;
    unsigned MaxBytesPerValue = 512;

    // Freeze instructions are problematic for value origin tracking.
    //
    // While `freeze poison` are intended to help optimization by allowing it to pick any value, we cannot just
    // treat `freeze poison` as UndefOrPoison, because an optimization relying on that would need to ensure
    // other users of the optimized `freeze poison` observe the same value picked by optimization, and value origin
    // tracking does not allow to query which `freeze poison` instructions a particular slice originates from.
    // Instead, the only safe way to treat `freeze poison` is dynamic.
    //
    // In some cases, e.g. when not optimizing based on the analysis result, and instead just using it for sanity
    // checking in testing, treating `freeze poison` as UndefOrPoison however is the intended result, and if
    // value origin tracking implicitly considered all `freeze poison` as dynamic, then client code would need to
    // propagate the intended UndefOrPoison semantics manually.
    //
    // The FreezeHandlingMode enum allows to avoid that, allowing the client to specify how `freeze poison` and
    // `freeze undef` should be handled.
    //
    // If we want to optimize based on `freeze poison`, one option would be eliminating all freeze instructions by some
    // constant (e.g. `zeroinitializer`) before running the analysis, as some LLVM transforms like instcombine do.
    // This ensures that not only the analysis sees a common constant value for `freeze poison`, but also ensures other
    // uses of `freeze poison` observe the same value.
    //
    // As a less conservative potential future improvement, we could instead explicitly keep track of FrozenPoison
    // slices in value origin tracking, and when merging FrozenPoison with constants, recording which `freeze poison`
    // values need to be replaced by which constants to allow that.
    enum class FreezeHandlingMode {
      // Treat slices in freeze instructions that are UndefOrPoison in the freeze operand as dynamic.
      Dynamic = 0,
      // Always forward value infos of freeze operands for freeze instructions.
      // In particular, `freeze poison` is always reported as UndefOrPoison.
      Forward
    };
    FreezeHandlingMode FreezeMode = FreezeHandlingMode::Dynamic;
  };

  using ValueInfo = ValueTracking::ValueInfo;
  // In some cases, client code has additional information on where values originate from, or
  // where they should be assumed to originate from just for the purpose of the analysis.
  // For instance, if a value is spilled and then re-loaded, value origin tracking
  // would consider the reloaded value as unknown dynamic, because it doesn't track memory.
  // Value origin assumptions allow the client to provide such extra information.
  // For each registered value, when the analysis reaches the given value, it will instead rely on the supplied
  // ValueInfo, and replace dynamic references by the analysis result for these dynamic values.
  // This means that when querying values for which assumptions were given, it is *not* ensured that
  // the exact assumptions are returned.
  //
  // Consider this example using dword slices:
  //    %equals.3 = add i32 3, 0
  //    %unknown = call i32 @opaque()
  //    %arr.0 = insertvalue [3 x i32] poison, i32 %equals.3, 0
  //    %arr.1 = insertvalue [3 x i32] %arr.0, i32 %unknown, 1
  //    %arr.stored = insertvalue [3 x i32] %arr.1, i32 %unknown, 2
  //    store [3 x i32] %arr.stored, ptr %ptr
  //    %reloaded = load [3 x i32], ptr %ptr
  // We supply the assumption that the first two dwords of %reloaded are in fact the first two dwords of
  // %arr.stored, and that the third dword equals 7 (because we have some additional knowledge somehow).
  // Then, when querying %reloaded, the result will be:
  //  * dword 0: constant: 0x3 (result of the add)
  //  * dword 1: dynamic: %unknown (offset 0)
  //  * dword 2: constant: 0x7
  //
  // If only some slices are known, the other slices can use the fallback of point to the value itself.
  // For values with assumptions, we skip the analysis we'd perform otherwise, so adding assumptions can
  // lead to worse analysis results on values that can be analyzed. For now, this feature however
  // is intended for values that are otherwise opaque. Support for merging with the standard analysis could be added.
  //
  // For now, only assumptions on instructions are supported.
  // The intended uses of this feature only require it for instructions, and support for non-instructions
  // is a bit more complicated but can be added if necessary.
  // Also, only a single status on assumptions is allowed.
  using ValueOriginAssumptions = llvm::DenseMap<llvm::Instruction *, ValueInfo>;

  ValueOriginTracker(const llvm::DataLayout &DL, Options Opts,
                     ValueOriginAssumptions OriginAssumptions = ValueOriginAssumptions{})
      : DL{DL}, Opts{Opts}, OriginAssumptions(std::move(OriginAssumptions)) {}

  // Computes a value info for the given value.
  // If the value has been seen before, returns a cache hit from the ValueInfos map.
  // When querying multiple values within the same functions, it is more efficient
  // to first run analyzeValues() on all of them together.
  ValueInfo getValueInfo(llvm::Value *V);

  // Analyze a set of values in bulk for efficiency.
  // Value analysis needs to process whole functions, so analysing multiple values within the same
  // function allows to use a single pass for them all.
  // The passed values don't have to be instructions, and don't have to be in the same functions,
  // although there is no perf benefit in that case.
  // Values may contain duplicates.
  void analyzeValues(llvm::ArrayRef<llvm::Value *> Values);

private:
  struct ValueInfoBuilder;
  const llvm::DataLayout &DL;
  Options Opts;
  ValueOriginAssumptions OriginAssumptions;
  llvm::DenseMap<llvm::Value *, ValueInfo> ValueInfos;

  // Analyze a value, creating a ValueInfo for it.
  // If V is an instruction, this assumes the ValueInfos of dependencies have
  // already been created. If some miss, we assume cyclic dependencies and give up
  // on this value.
  ValueInfo computeValueInfo(llvm::Value *V);
  // Same as above, implementing constant analysis
  ValueInfo computeConstantValueInfo(ValueInfoBuilder &VIB, llvm::Constant *C);
  // Given an origin assumption, compute a value info that combines analysis results
  // of the values referenced by the assumption.
  ValueInfo computeValueInfoFromAssumption(ValueInfoBuilder &VIB, const ValueInfo &OriginAssumption);

  // Implementation function for analyzeValues():
  // Ensures that the ValueInfos map contains an entry for V, by optionally computing a value info first.
  // Then, return a reference to the value info object within the map.
  // The resulting reference is invalidated if ValueInfos is mutated.
  // Assumes that all values this depends on have already been analyzed, except for phi nodes,
  // which are handled pessimistically in case of loops.
  ValueInfo &getOrComputeValueInfo(llvm::Value *V, bool KnownToBeNew = false);
};

} // namespace CompilerUtils
