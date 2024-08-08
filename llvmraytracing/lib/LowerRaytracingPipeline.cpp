/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- LowerRaytracingPipeline.cpp -----------------===//
//
// This file implements the frontend part for coroutine support for lgc.rt ops.
// - Add a global for the continuation stack pointer.
// - Introduce a global for the payload.
// - Replace traceRay or callShader function calls with a compiler generated
//   code snippet. The snippets call setup and teardown hooks and calls await to
//   mark the continuation point
// - Convert the incoming payload from an argument into a local stack variable,
//   loaded from the global payload.
// - For incoming payload with a memory part, save the memory pointer if the
//   global payload is overwritten in the function.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llpc/GpurtEnums.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "llvmraytracing/PayloadAccessQualifiers.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/OpSet.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#define DEBUG_TYPE "lower-raytracing-pipeline"

using namespace llvm;
using namespace lgc::cps;
using namespace lgc::rt;

namespace {

// Create a GEP if I is non-null, otherwise return the pointer.
static Value *SimplifyingCreateConstGEP1_32(IRBuilder<> &B, Type *Ty, Value *Ptr, uint32_t I) {
  // A GEP with a single zero index is redundant with opaque pointers
  if (I == 0)
    return Ptr;
  return B.CreateConstGEP1_32(Ty, Ptr, I);
}

static Value *SimplifyingCreateConstInBoundsGEP1_32(IRBuilder<> &B, Type *Ty, Value *Ptr, uint32_t I) {
  // A GEP with a single zero index is redundant with opaque pointers
  if (I == 0)
    return Ptr;
  return B.CreateConstInBoundsGEP1_32(Ty, Ptr, I);
}

// Helper struct to avoid recursively passing these arguments
struct PayloadCopyHelper {
  Module &M;
  IRBuilder<> &B;
  Type &PayloadTy;
  Value *LocalPayload;
  std::optional<PAQShaderStage> Stage;
  PAQAccessKind GlobalAccessKind;
  // Index into (nested) fields of the payload,
  // filled recursively
  SmallVector<Value *> PayloadIdxList;
  // Used to avoid duplicate copies when importing ClosestHitOut + MissOut
  SmallDenseSet<const PAQNode *, 16> *CopiedNodes;
  Value *Serialization;
  const PAQSerializationLayout *Layout;
  // Pointer to the spilled payload (loaded from LocalPayload)
  Value *SpilledPayloadPtr;
  // Number of registers/dwords that are stored in registers
  uint32_t PayloadRegisterCount;

  void copyPayloadRecursively(const PAQNode *Node) {
    if (CopiedNodes && CopiedNodes->contains(Node)) {
      // Already copied in previous run, nothing to do.
      return;
    }

    auto It = Layout->NodeStorageInfos.find(Node);
    if (It == Layout->NodeStorageInfos.end()) {
      // This node is not directly represented in the payload serialization
      // struct, recursively traverse nested fields
      for (unsigned I = 0; I < Node->Children.size(); ++I) {
        PayloadIdxList.push_back(B.getInt32(I));
        copyPayloadRecursively(&Node->Children[I]);
        PayloadIdxList.pop_back();
      }
      return;
    }
    // This node corresponds to a field in the payload serialization struct

    // Check if field has access qualifiers set, i.e. is copied from/to global
    if (Stage && !Node->AccessMask->get(Stage.value(), GlobalAccessKind)) {
      return;
    }

    copyField(Node->Ty, It->second.IndexIntervals);

    // Register node as copied
    if (CopiedNodes)
      CopiedNodes->insert(Node);
  }

  // Perform copy for each index interval (i.e, for each contiguous range of
  // storage memory)
  void copyField(Type *FieldTy, const PAQIndexIntervals &Intervals) {
    // Pointer to the node field in the local payload
    auto *LocalFieldPtr = B.CreateInBoundsGEP(&PayloadTy, LocalPayload, PayloadIdxList);

    // Counts how many bytes have already been copied
    unsigned FieldByteOffset = 0;
    unsigned FieldNumBytes = M.getDataLayout().getTypeStoreSize(FieldTy).getFixedValue();

    for (auto [IntervalIdx, CompleteInterval] : enumerate(Intervals)) {
      copyFieldInterval(LocalFieldPtr, &FieldByteOffset, FieldNumBytes, CompleteInterval);
    }

    assert(FieldByteOffset == FieldNumBytes && "Inconsistent storage size!");
  }

  void copyFieldInterval(Value *LocalFieldPtr, unsigned *FieldByteOffset, unsigned FieldNumBytes,
                         const PAQIndexInterval CompleteInterval) {
    auto *I32 = Type::getInt32Ty(M.getContext());
    // Split interval into registers and memory part.
    // Map an interval to its register or memory pointer.
    SmallVector<std::pair<PAQIndexInterval, Value *>, 2> TmpIntervals;

    if (CompleteInterval.Begin < PayloadRegisterCount) {
      PAQIndexInterval Interval = {CompleteInterval.Begin, std::min(CompleteInterval.End, PayloadRegisterCount)};
      // Pointer to start of current interval in global payload
      auto *GlobalIntervalI32Ptr = SimplifyingCreateConstInBoundsGEP1_32(B, I32, Serialization, Interval.Begin);
      TmpIntervals.push_back({Interval, GlobalIntervalI32Ptr});
    }
    if (CompleteInterval.End > PayloadRegisterCount) {
      PAQIndexInterval Interval = {std::max(CompleteInterval.Begin, PayloadRegisterCount), CompleteInterval.End};
      // Pointer to start of current interval in global payload
      auto *GlobalIntervalI32Ptr =
          SimplifyingCreateConstInBoundsGEP1_32(B, I32, SpilledPayloadPtr, Interval.Begin - PayloadRegisterCount);
      TmpIntervals.push_back({Interval, GlobalIntervalI32Ptr});
    }

    for (auto [Interval, GlobalIntervalI32Ptr] : TmpIntervals) {
      // Obtain i32-based index from byte-offset. We only expect
      // to increase FieldByteOffset by a non-multiple of RegisterBytes
      // in the last iteration, so here it should always be divisible
      unsigned FieldI32Offset = *FieldByteOffset / RegisterBytes;
      assert(*FieldByteOffset == FieldI32Offset * RegisterBytes);
      // I32 pointer into field, offset by FieldI32Offset
      auto *FieldIntervalI32Ptr = SimplifyingCreateConstInBoundsGEP1_32(B, I32, LocalFieldPtr, FieldI32Offset);

      // Determine Src and Dst
      auto *Src = FieldIntervalI32Ptr;
      auto *Dst = GlobalIntervalI32Ptr;
      if (GlobalAccessKind != PAQAccessKind::Write)
        std::swap(Src, Dst);

      unsigned NumCopyBytes = RegisterBytes * Interval.size();

      unsigned FieldNumRemainingBytes = FieldNumBytes - *FieldByteOffset;
      if (NumCopyBytes > FieldNumRemainingBytes) {
        NumCopyBytes = FieldNumRemainingBytes;
      }

      copyBytes(B, Dst, Src, NumCopyBytes);
      *FieldByteOffset += NumCopyBytes;
    }
  }
};

enum class ContinuationCallType {
  Traversal,
  CallShader,
  AnyHit,
};

class ModuleMetadataState final {
public:
  ModuleMetadataState(llvm::Module &Module);
  ModuleMetadataState(const ModuleMetadataState &) = delete;
  ModuleMetadataState(ModuleMetadataState &&) = default;

  uint32_t getMaxPayloadRegisterCount() const { return MaxPayloadRegisterCount; }

  std::optional<uint32_t> tryGetPreservedPayloadRegisterCount() const { return PreservedPayloadRegisterCount; }

  void updateMaxUsedPayloadRegisterCount(uint32_t Count) {
    MaxUsedPayloadRegisterCount = std::max(Count, MaxUsedPayloadRegisterCount);
  }

  uint32_t getMaxUsedPayloadRegisterCount() const { return MaxUsedPayloadRegisterCount; }

  uint32_t getMaxHitAttributeByteCount() const { return MaxHitAttributeByteCount; }

  bool isInLgcCpsMode() const { return IsInLgcCpsMode; }

  void updateModuleMetadata() const;

private:
  Module &Mod;
  /// MaxPayloadRegisterCount is initialized from metadata. If there is none,
  /// use this default instead:
  static constexpr uint32_t DefaultPayloadRegisterCount = 30;
  /// [In]: Maximum allowed number of registers to be used for the payload.
  ///       It is guaranteed that all modules in a pipeline share this value.
  uint32_t MaxPayloadRegisterCount = 0;
  /// [In]: If known, the number of payload registers that need to be preserved
  ///       by functions that don't know the payload type, e.g. Traversal.
  std::optional<uint32_t> PreservedPayloadRegisterCount = {};
  /// [Out]: The maximum number of payload registers written or read by any
  ///        shader in the module. This excludes intersection shaders, which
  ///        just pass through an existing payload.
  uint32_t MaxUsedPayloadRegisterCount = 0;
  /// [In]: The maximum size of hit attribute stored on the module as metadata.
  uint32_t MaxHitAttributeByteCount = 0;
  /// [In]: The address space used for the continuations stack.
  ///       Either stack or global memory.
  ContStackAddrspace StackAddrspace = ContHelper::DefaultStackAddrspace;

  /// If the module has lgc.cps.module metadata attached.
  bool IsInLgcCpsMode = false;
};

class LowerRaytracingPipelinePassImpl final {
public:
  LowerRaytracingPipelinePassImpl(Module &M, Module &GpurtLibrary);
  PreservedAnalyses run();

private:
  struct FunctionData {
    RayTracingShaderStage Kind = RayTracingShaderStage::Count;
    SmallVector<CallInst *> TraceRayCalls;
    SmallVector<CallInst *> ReportHitCalls;
    SmallVector<CallInst *> CallShaderCalls;
    /// Calls to hlsl intrinsics that cannot be rematerialized
    SmallVector<CallInst *> IntrinsicCalls;
    SmallVector<CallInst *> ShaderIndexCalls;
    SmallVector<CallInst *> ShaderRecordBufferCalls;
    SmallVector<JumpOp *> JumpCalls;

    /// Pointer to the alloca'd system data object in this function
    AllocaInst *SystemData = nullptr;
    StructType *SystemDataTy = nullptr;
    /// The first store to the alloca'd system data.
    Instruction *SystemDataFirstStore = nullptr;
    Type *ReturnTy = nullptr;

    /// Storage for the spilled payload, which is put into the continuation
    /// state and stored on the stack.
    AllocaInst *SpilledPayload = nullptr;
    /// Maximum number of I32s required to store the outgoing payload in all
    /// CallShader or TraceRay (maximum over all TraceRay formats) calls
    uint32_t MaxOutgoingPayloadI32s = 0;
    /// Size of the CPS stack allocation used for spilled parts of the payload.
    /// This size is large enough for all used outgoing payload types.
    int32_t PayloadSpillSize = 0;
    /// Type of the incoming payload
    Type *IncomingPayload = nullptr;
    /// Serialization info for the incoming payload, if there is one.
    /// Also applies to the outgoing payload in that case.
    PAQSerializationInfoBase *IncomingPayloadSerializationInfo = nullptr;
    /// hit attributes type, incoming for AnyHit and ClosestHit, outgoing for
    /// Intersection
    Type *HitAttributes = nullptr;

    /// The payload storage and its type belongs to this function.
    Value *PayloadStorage = nullptr;
    Type *PayloadStorageTy = nullptr;
    /// The starting dword of payload storage argument. If there is no payload
    /// argument, this is std::nullopt.
    std::optional<uint32_t> FirstPayloadArgumentDword = std::nullopt;
    // For shaders that pass through a payload (e. g. intersection and
    // traversal), use this value to indicate the number of passed-through
    // payload dwords.
    std::optional<uint32_t> NumPassedThroughPayloadDwords;
  };

  /// Needed data for handling the end of a function
  struct FunctionEndData {
    Instruction *Terminator = nullptr;
    const PAQSerializationLayout *OutgoingSerializationLayout = nullptr;
    SmallVector<Value *> SavedRegisterValues;
    Value *NewPayload = nullptr;
    std::optional<PAQShaderStage> ShaderStage;
    Value *HitAttrsAlloca = nullptr;
    Value *OrigHitAttrsAlloca = nullptr;
    Type *NewRetTy = nullptr;
  };

  // Simplify some code used to compute and append padding and payload on
  // function signatures and continue / jump calls.
  class PayloadHelper final {
  public:
    PayloadHelper(Module &Mod, const DataLayout &DL, llvm_dialects::Builder &Builder, bool CpsMode)
        : Mod{Mod}, DL{DL}, Builder{Builder}, IsCpsMode{CpsMode} {}

    /// Append padding and payload to lgc.cps.jump calls.
    void patchJumpCalls(Function *Parent, ArrayRef<JumpOp *> JumpCalls, std::optional<uint32_t> PayloadStartDword) {
      if (!IsCpsMode || !PayloadStartDword.has_value())
        return;

      for (auto *Jump : JumpCalls) {
        Builder.SetInsertPoint(Jump);
        SmallVector<Value *> NewTailArgs(Jump->getTail());

        // Add padding so that payload starts at a fixed dword.
        ContHelper::addPaddingValue(DL, Parent->getContext(), NewTailArgs, PayloadStartDword.value());
        // Insert payload into tail args.
        NewTailArgs.push_back(Parent->getArg(CpsArgIdxPayload));

        Jump->replaceTail(NewTailArgs);
      }
    }

    /// Find a continue call starting from the terminator of a given basic
    /// block.
    /// Returns a pair containing a pointer to the call, and the iterator range
    /// containing the tail argument list used, for computing the padding at the
    /// callsite.
    std::pair<CallInst *, llvm::iterator_range<llvm::User::value_op_iterator>>
    getContinueCallFromTerminator(Instruction *Terminator) {
      assert((isa<UnreachableInst, ReturnInst>(Terminator)));
      auto RIt = Terminator->getReverseIterator();

      // We technically could have an eligible terminator
      // as the single instruction of a BB, so we don't want to assert here.
      BasicBlock *BB = Terminator->getParent();

      // Find a continue call starting from the unreachable.
      // Don't single-step because at this point the caller
      // has created the payload load before the terminator,
      // and re-creating the continue call will fix up the order again.
      CallInst *CInst = nullptr;
      while (RIt != BB->rend()) {
        CInst = dyn_cast<CallInst>(&*RIt);

        if (CInst)
          break;

        ++RIt;
      }

      assert(CInst);

      if (auto *Continue = dyn_cast<lgc::ilcps::ContinueOp>(CInst))
        return {Continue, Continue->getTail()};

      if (auto *WaitContinue = dyn_cast<lgc::ilcps::WaitContinueOp>(CInst))
        return {WaitContinue, WaitContinue->getTail()};

      report_fatal_error("LowerRaytracingPipelinePassImpl::PayloadHelper::"
                         "getContinueCallFromTerminator: expected either a "
                         "lgc.ilcps.continue or a lgc.ilcps.waitContinue op!");
    }

    /// Create and initialize payload serialization storage for non-Traversal
    /// shader.
    void initializePayloadSerializationStorage(Function *Parent, FunctionData &Data) {
      llvm_dialects::Builder::InsertPointGuard Guard{Builder};
      Builder.SetInsertPointPastAllocas(Parent);
      Data.PayloadStorage = Builder.CreateAlloca(Data.PayloadStorageTy);
      Data.PayloadStorage->setName("payload.serialization.alloca");
      // TODO: We shouldn't need to create the alloca for RGS.
      if (Data.Kind != RayTracingShaderStage::RayGeneration && Data.FirstPayloadArgumentDword.has_value())
        Builder.CreateStore(Parent->getArg(Parent->arg_size() - 1), Data.PayloadStorage);
    }

    Type *getPayloadStorageTy(uint32_t MaxPayloadRegisterCount, FunctionData &Data) {
      uint32_t PayloadStorageI32s = 0;
      if (Data.NumPassedThroughPayloadDwords.has_value()) {
        PayloadStorageI32s = Data.NumPassedThroughPayloadDwords.value();
      } else {
        // Take (for RGS) the maximum outgoing payload, otherwise take the max
        // with the serialized incoming payload info.
        PayloadStorageI32s = Data.MaxOutgoingPayloadI32s;
        if (Data.IncomingPayloadSerializationInfo)
          PayloadStorageI32s = std::max(PayloadStorageI32s, Data.IncomingPayloadSerializationInfo->MaxStorageI32s);
      }
      return ArrayType::get(Builder.getInt32Ty(), PayloadStorageI32s);
    }

    /// Compute the dword at which payload starts in the argument at most in the
    /// argument list.
    std::optional<uint32_t> getPayloadStartDword(FunctionData &Data, uint32_t MaxHitAttributeBytes,
                                                 Type *TraversalDataTy) {
      if (Data.PayloadStorageTy->getArrayNumElements() == 0)
        return std::nullopt;

      assert(TraversalDataTy && "Failed to detect traversal system data type");

      // For lgc.cps mode, take into account that the return address and shader
      // index dwords are inserted at a later stage.
      // Always ensure that we consider the two dword barycentric coordinates
      // passed as argument for _AmdEnqueueAnyHit calls.
      return (IsCpsMode ? 1 + 1 : 0) + getArgumentDwordCount(DL, TraversalDataTy) +
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 503627
             // Old version of the code
             std::max(divideCeil(MaxHitAttributeBytes, RegisterBytes), uint64_t(2));
#else
             // New version of the code (also handles unknown version, which we
             // treat as latest)
             std::max(divideCeil(MaxHitAttributeBytes, RegisterBytes), 2u);
#endif
    }

    /// Compute padding and payload arguments based on the passed arguments and
    /// append them to ArgTys.
    /// Returns a pair (paddingType, payloadType).
    std::pair<Type *, Type *> computePaddingAndPayloadArgTys(SmallVectorImpl<Type *> &ArgTys,
                                                             uint32_t PayloadSizeDwords,
                                                             std::optional<uint32_t> PayloadStartDword,
                                                             uint32_t Offset = 0) {
      Type *PaddingTy = nullptr;
      const uint32_t ShiftedStartDword = PayloadStartDword.has_value() ? PayloadStartDword.value() - Offset : 0;

#ifndef NDEBUG
      LLVM_DEBUG(dbgs() << "Computing padding and payload based on following data:\n"
                        << "Payload size: " << PayloadSizeDwords << " dwords\n"
                        << "Payload start dword: " << ShiftedStartDword << "\nArgument types:\n");
      for (Type *Ty : ArgTys)
        LLVM_DEBUG(dbgs() << *Ty << ": " << lgc::cps::getArgumentDwordCount(DL, Ty) << " dwords\n");
#endif

      // Compute padding type so that payload starts at a fixed dword.
      // If PayloadStartDword is set to std::nullopt, then we don't pass
      // payload, thus we don't need padding.
      if (PayloadStartDword.has_value()) {
        PaddingTy = ContHelper::getPaddingType(DL, Mod.getContext(), ArgTys, ShiftedStartDword);
      } else {
        assert(PayloadSizeDwords == 0 && "PayloadHelper::computePaddingAndPayloadArgTys: Expected zero "
                                         "payload dwords!");
        PaddingTy = ArrayType::get(Builder.getInt32Ty(), 0);
      }

      Type *PayloadTy = ArrayType::get(Builder.getInt32Ty(), PayloadSizeDwords);

#ifndef NDEBUG
      LLVM_DEBUG(dbgs() << "Resulting padding type: " << *PaddingTy << "\nResulting payload type: " << *PayloadTy
                        << "\n---\n");
#endif

      ArgTys.push_back(PaddingTy);
      ArgTys.push_back(PayloadTy);

      return {PaddingTy, PayloadTy};
    }

    /// Append the actual padding and payload arguments to a jump or continue
    /// call. Uses PaddingArgs to compute the padding, loads the payload from
    /// the PayloadSerializationStorage and appends both to the OutArgList.
    void appendPaddingAndPayloadValues(SmallVectorImpl<Value *> &PaddingArgs, SmallVectorImpl<Value *> &OutArgList,
                                       uint32_t OutgoingPayloadRegisterCount, std::optional<uint32_t> PayloadStartDword,
                                       Value *PayloadSerializationStorage) {

      if (!PayloadStartDword.has_value())
        return;

      ContHelper::addPaddingValue(DL, Mod.getContext(), PaddingArgs, PayloadStartDword.value());

      OutArgList.push_back(PaddingArgs.back());

      OutArgList.push_back(Builder.CreateLoad(ArrayType::get(Builder.getInt32Ty(), OutgoingPayloadRegisterCount),
                                              PayloadSerializationStorage));
    }

  private:
    Module &Mod;
    const DataLayout &DL;
    llvm_dialects::Builder &Builder;
    bool IsCpsMode = false;
  };

  void replaceCall(FunctionData &Data, CallInst *Call, Function *Func, ContinuationCallType CallType);
  void handleRestoreSystemData(CallInst *Call);
  void handleExitRayGen(const FunctionData &Data);
  void replaceContinuationCall(ContinuationCallType CallType, CallInst *Call, const FunctionData &Data,
                               Value *PayloadOrAttrs, Type *PayloadOrAttrsTy);
  void replaceReportHitCall(FunctionData &Data, CallInst *Call);

  void replaceShaderIndexCall(FunctionData &Data, CallInst *Call);
  void replaceShaderRecordBufferCall(FunctionData &Data, CallInst *Call);

  void handleGetShaderKind(Function &Func);
  void handleGetCurrentFuncAddr(Function &Func);

  void handleAmdInternalFunc(Function &Func);

  void splitRestoreBB();

  void handleUnrematerializableCandidates();

  void collectGpuRtFunctions();

  // Computes an upper bound on the number of required payload registers
  // for a TraceRay call, based on module-wide max attribute and payload size.
  // In lgc.cps mode, this determines the number of payload
  // registers preserved by Intersection shaders.
  // Doesn't apply to callable shaders.
  unsigned getUpperBoundOnTraceRayPayloadRegisters() const;

  // Copy the payload content between (global) payload storage and local
  // payload. Excludes the stack pointer or hit attributes which may also reside
  // in payload storage. If Stage is not set, all fields in SerializationInfo
  // are copied. Used for CallShader accesses which are not PAQ qualified and do
  // not have PAQShaderStage values. If CopiedNodes is set, nodes contained will
  // not be copied, and all copied nodes are added to it.
  void copyPayload(Type &PayloadTy, Value *LocalPayload, Value *PayloadStorage, std::optional<PAQShaderStage> Stage,
                   PAQAccessKind GlobalAccessKind, const PAQSerializationLayout &Layout,
                   SmallDenseSet<const PAQNode *, 16> *CopiedNodes = nullptr);

  // Special handling for case of copying the result payload of a traceray call
  // back to the local payload of the caller.
  // This is needed to implement the ClosestHitOut/MissOut optimization.
  // We first perform a copy using the ClosestHitOut layout, and then perform an
  // additional copy using the MissOut layout, skipping any fields already
  // copied (i.e. only copying write(miss) : read(caller) fields).
  void copyTraceRayPayloadIncomingToCaller(const PAQTraceRaySerializationInfo &PAQSerializationInfo,
                                           Value *LocalPayload, Value *PayloadStorage);

  // Caller-save payload registers before CallShader() or TraceRay(),
  // which can override payload registers. A register needs to be saved
  // if it is live in OutgoingLayout, and not written in OutgoingLayout.
  // This includes the payload memory pointer if present.
  // SavedRegisters maps indices of payload registers to their saved values.
  void savePayloadRegistersBeforeRecursion(Value *PayloadStorage, RayTracingShaderStage Kind,
                                           const PAQSerializationLayout &IncomingLayout,
                                           const PAQSerializationLayout &OutgoingLayout,
                                           SmallVectorImpl<Value *> &SavedRegisterValues);

  // Restore previously saved registers.
  void restorePayloadRegistersAfterRecursion(Value *PayloadStorage,
                                             const SmallVectorImpl<Value *> &SavedRegisterValues);

  // Sets register count metadata (incoming on entry functions, outgoing on
  // continue calls) in GpuRt entries (Traversal and launch kernel).
  void setGpurtEntryRegisterCountMetadata();

  void copyHitAttributes(FunctionData &Data, Value *SystemData, Type *SystemDataTy, Value *LocalHitAttributes,
                         bool GlobalToLocal, const PAQSerializationLayout *Layout);
  void processContinuations();
  void processFunctionEntry(FunctionData &Data, Argument *SystemDataArgument);
  void processFunctionEnd(FunctionData &Data, FunctionEndData &EData);
  void processFunction(Function *F, FunctionData &FuncData);
  void handleContPayloadRegisterI32Count(Function &F);
  void handleContPayloadRegistersGetI32(Function &F, Function &Parent, FunctionData &Data);
  void handleContPayloadRegistersSetI32(Function &F, Function &Parent, FunctionData &Data);

  void collectProcessableFunctions();

  Instruction *insertCpsAwait(Type *ReturnTy, Value *ShaderAddr, Instruction *Call, ArrayRef<Value *> Args,
                              ContinuationCallType CallType, RayTracingShaderStage ShaderStage);

  MapVector<Function *, FunctionData> ToProcess;
  Module *Mod;
  Module *GpurtLibrary;
  LLVMContext *Context;
  const DataLayout *DL;
  llvm_dialects::Builder Builder;
  ModuleMetadataState MetadataState;
  PAQSerializationInfoManager PAQManager;
  PayloadHelper PayloadHelper;
  CompilerUtils::CrossModuleInliner CrossInliner;
  Type *I32;
  Type *TokenTy;
  /// System data type passed to Traversal
  Type *TraversalDataTy;
  /// System data type passed to ClosestHit and Miss
  Type *HitMissDataTy;
  /// Dispatch system data type passed to RayGen and others
  Type *DispatchSystemDataTy;

  // Function definitions and declarations from HLSL
  // Driver implementation that returns if AcceptHitAndEndSearch was called
  Function *IsEndSearch;
  // Driver implementations to set and get the triangle hit attributes from
  // system data
  Function *GetTriangleHitAttributes;
  Function *SetTriangleHitAttributes;
  Function *GetLocalRootIndex;
  Function *SetLocalRootIndex;
  Function *ExitRayGen;
  Function *TraceRay;
  Function *CallShader;
  Function *ReportHit;
  Function *AcceptHit;
  Function *GetSbtAddress;
  Function *GetSbtStride;
  MapVector<Type *, Function *> ShaderStartOverloads;
};

} // namespace

constexpr unsigned ModuleMetadataState::DefaultPayloadRegisterCount;

ModuleMetadataState::ModuleMetadataState(Module &Module) : Mod{Module} {
  // Import PayloadRegisterCount from metadata if set,
  // otherwise from default
  auto RegisterCountFromMD = ContHelper::MaxPayloadRegisterCount::tryGetValue(&Module);
  MaxPayloadRegisterCount = RegisterCountFromMD.value_or(DefaultPayloadRegisterCount);

  // Check that if there is a required minimum number of payload registers,
  // it is compatible
  PreservedPayloadRegisterCount = ContHelper::PreservedPayloadRegisterCount::tryGetValue(&Module);
  assert(PreservedPayloadRegisterCount.value_or(MaxPayloadRegisterCount) <= MaxPayloadRegisterCount);

  MaxUsedPayloadRegisterCount = ContHelper::MaxUsedPayloadRegisterCount::tryGetValue(&Module).value_or(0);
  if (PreservedPayloadRegisterCount.has_value())
    MaxUsedPayloadRegisterCount = std::max(MaxUsedPayloadRegisterCount, PreservedPayloadRegisterCount.value());

  // Use max hit attribute size from metadata, or use globally max allowed
  // value for the max if metadata is not set
  MaxHitAttributeByteCount = getMaxHitAttributeSize(&Mod).value_or(GlobalMaxHitAttributeBytes);

  if (MaxHitAttributeByteCount % RegisterBytes != 0) {
    auto AlignedMaxHitAttributeSize = alignTo(MaxHitAttributeByteCount, RegisterBytes);
    LLVM_DEBUG(dbgs() << "Aligning misaligned max hit attribute size " << MaxHitAttributeByteCount << " to "
                      << AlignedMaxHitAttributeSize << "\n");
    MaxHitAttributeByteCount = AlignedMaxHitAttributeSize;
  }

  // Import StackAddrspace from metadata if set, otherwise from default
  auto StackAddrspaceMD = ContHelper::tryGetStackAddrspace(Module);
  StackAddrspace = StackAddrspaceMD.value_or(ContHelper::DefaultStackAddrspace);

  IsInLgcCpsMode = ContHelper::isLgcCpsModule(Mod);
}

/// Write the previously derived information about max payload registers and
/// stack address space that was derived by metadata as global state.
void ModuleMetadataState::updateModuleMetadata() const {
  ContHelper::MaxPayloadRegisterCount::setValue(&Mod, MaxPayloadRegisterCount);
  ContHelper::MaxUsedPayloadRegisterCount::setValue(&Mod, MaxUsedPayloadRegisterCount);
  ContHelper::setStackAddrspace(Mod, StackAddrspace);
}

// Create a lgc.cps.await operation for a given shader address.
Instruction *LowerRaytracingPipelinePassImpl::insertCpsAwait(Type *ReturnTy, Value *ShaderAddr, Instruction *Call,
                                                             ArrayRef<Value *> Args, ContinuationCallType CallType,
                                                             RayTracingShaderStage ShaderStage) {
  Builder.SetInsertPoint(Call);

  Value *CR = nullptr;
  if (ShaderAddr->getType()->getIntegerBitWidth() == 64)
    CR = Builder.CreateTrunc(ShaderAddr, Type::getInt32Ty(Mod->getContext()));
  else
    CR = ShaderAddr;

  RayTracingShaderStage CallStage = RayTracingShaderStage::Count;
  if (CallType == ContinuationCallType::Traversal)
    CallStage = RayTracingShaderStage::Traversal;
  else if (CallType == ContinuationCallType::CallShader)
    CallStage = RayTracingShaderStage::Callable;
  else if (CallType == ContinuationCallType::AnyHit)
    CallStage = RayTracingShaderStage::AnyHit;

  assert(CallStage != RayTracingShaderStage::Count && "LowerRaytracingPipelinePassImpl::insertCpsAwait: Invalid "
                                                      "call stage before inserting lgc.cps.await operation!");

  return Builder.create<AwaitOp>(ReturnTy, CR, 1 << static_cast<uint8_t>(getCpsLevelForShaderStage(CallStage)), Args);
}

Function *llvm::getSetLocalRootIndex(Module &M) {
  auto *Name = "amd.dx.setLocalRootIndex";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *I32 = Type::getInt32Ty(C);
  auto *FuncTy = FunctionType::get(Void, {I32}, false);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoFree, Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
}

// Set maximum continuation stack size metadata
static void setStacksizeMetadata(Function &F, uint64_t NeededStackSize) {
  uint64_t CurStackSize = ContHelper::StackSize::tryGetValue(&F).value_or(0);
  if (NeededStackSize > CurStackSize)
    ContHelper::StackSize::setValue(&F, NeededStackSize);
}

// Create an ExtractElement instruction for each index of a FixedVector @Vector
// and return it.
static SmallVector<Value *, 3> flattenVectorArgument(IRBuilder<> &B, Value *Vector) {
  assert(isa<FixedVectorType>(Vector->getType()) && "Not a FixedVectorType!");

  SmallVector<Value *, 3> Arguments;

  for (unsigned Idx = 0; Idx < cast<FixedVectorType>(Vector->getType())->getNumElements(); ++Idx) {
    Arguments.push_back(B.CreateExtractElement(Vector, B.getInt32(Idx)));
  }

  return Arguments;
}

// Check if @Arg is of fixed vector type. If yes, flatten it into extractelement
// instructions and append them to @Arguments. Return true if @Arguments
// changed, false otherwise.
static bool flattenVectorArgument(IRBuilder<> &B, Value *Arg, SmallVectorImpl<Value *> &Arguments) {
  if (isa<FixedVectorType>(Arg->getType())) {
    const auto &FlattenedArguments = flattenVectorArgument(B, Arg);
    if (!FlattenedArguments.empty()) {
      Arguments.append(FlattenedArguments.begin(), FlattenedArguments.end());

      return true;
    }
  }

  return false;
}

/// Clone a function and replace a call with a call to the cloned function
void LowerRaytracingPipelinePassImpl::replaceCall(FunctionData &Data, CallInst *Call, Function *Func,
                                                  ContinuationCallType CallType) {
  Builder.SetInsertPoint(Call);
  auto *AfterCall = &*++Builder.GetInsertPoint();
  auto *SystemDataTy = getFuncArgPtrElementType(Func, 0);
  Value *PayloadOrAttrs = nullptr;

  SmallVector<Value *, 17> Arguments;
  Arguments.push_back(getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, cast<StructType>(SystemDataTy)));

  // Construct the new argument list for the driver-side call from a lgc.rt
  // dialect op. This requires some special handling since we cannot pass all
  // arguments directly (e. g. vector arguments), and we don't want to add all
  // arguments.
  switch (CallType) {
  // Handling a lgc.rt.trace.ray call.
  case ContinuationCallType::Traversal: {
    // Generally exclude the last (PAQ) argument.
    const unsigned ArgCount = Call->arg_size();
    for (unsigned CallI = 0; CallI < ArgCount - 2; ++CallI) {
      // For trace.ray calls, we need to flatten all vectors in the argument
      // list.
      Value *Arg = Call->getArgOperand(CallI);
      if (flattenVectorArgument(Builder, Arg, Arguments))
        continue;

      Arguments.push_back(Arg);
    }
    PayloadOrAttrs = Call->getArgOperand(Call->arg_size() - 2);

    break;
  }
  // Replacing a lgc.rt.report.hit or lgc.rt.call.callable.shader call.
  case ContinuationCallType::CallShader:
  case ContinuationCallType::AnyHit:
    // For the report.hit operation, we remove the PAQ size attribute since it
    // is included in the name. For the call.callable.shader operation, we
    // remove the PAQ size attribute as well since it is not supported.
    Arguments.append(Call->arg_begin(), Call->arg_end() - 2);
    PayloadOrAttrs = Call->getArgOperand(Call->arg_size() - 2);
    break;
  }

  // Get payload argument
  Type *PayloadOrAttrsTy = ContHelper::getPayloadTypeFromMetadata(*Call);
  auto *NewCall = Builder.CreateCall(Func, Arguments);

  if (!Call->getType()->isVoidTy())
    Call->replaceAllUsesWith(NewCall);
  Call->eraseFromParent();
  auto NewBlocks = CrossInliner.inlineCall(*NewCall);

  // Find special calls. Collect before replacing because replacing them inlines
  // functions and changes basic blocks.
  SmallVector<CallInst *> AwaitCalls;
  SmallVector<CallInst *> AcceptHitAttrsCalls;
  for (auto &BB : NewBlocks) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        auto *Callee = CI->getCalledFunction();
        if (!Callee)
          continue;
        auto FuncName = Callee->getName();
        if (FuncName.starts_with("_AmdAwait") || FuncName.starts_with("_AmdWaitAwait")) {
          AwaitCalls.push_back(CI);
        } else if (FuncName.starts_with("_AmdAcceptHitAttributes")) {
          AcceptHitAttrsCalls.push_back(CI);
        }
      }
    }
  }

  for (auto *CI : AwaitCalls) {
    Builder.SetInsertPoint(CI);
    replaceContinuationCall(CallType, CI, Data, PayloadOrAttrs, PayloadOrAttrsTy);
  }

  for (auto *CI : AcceptHitAttrsCalls) {
    // Commit hit attributes
    Builder.SetInsertPoint(CI);
    assert(TraversalDataTy != 0 && "Missing traversal system data!");
    copyHitAttributes(Data, CI->getArgOperand(0), TraversalDataTy, PayloadOrAttrs, false, nullptr);
    // Make sure that we store the hit attributes into the correct system
    // data (just in case dxc copied them around).
    assert(CI->getArgOperand(0) == Arguments[0] && "AcceptHitAttributes does not take the correct system data as "
                                                   "argument!");
    CI->eraseFromParent();
  }
  Builder.SetInsertPoint(AfterCall);
}

void LowerRaytracingPipelinePassImpl::handleRestoreSystemData(CallInst *Call) {
  // Store system data
  auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(Call->getCalledFunction(), 0));
  auto *SystemData = Call->getArgOperand(0);

  // Set local root signature on re-entry
  auto *LocalIndexSystemDataTy = cast<StructType>(getFuncArgPtrElementType(GetLocalRootIndex, 0));
  auto *LocalIndexSystemData = getDXILSystemData(Builder, SystemData, SystemDataTy, LocalIndexSystemDataTy);

  auto Stage = getLgcRtShaderStage(Call->getFunction());
  Value *LocalIndex = nullptr;
  if (Stage == RayTracingShaderStage::RayGeneration)
    LocalIndex = Builder.getInt32(0);
  else
    LocalIndex = CrossInliner.inlineCall(Builder, GetLocalRootIndex, LocalIndexSystemData).returnValue;
  LocalIndex->setName("local.root.index");
  Builder.CreateCall(SetLocalRootIndex, LocalIndex);
}

/// Replace a call to lgc.rt.report.hit with a call to the driver
/// implementation.
void LowerRaytracingPipelinePassImpl::replaceReportHitCall(FunctionData &Data, CallInst *Call) {
  assert(ReportHit && "ReportHit not found");

  replaceCall(Data, Call, ReportHit, ContinuationCallType::AnyHit);

  // Check if the search ended and return from Intersection if this is the case
  assert(IsEndSearch && "IsEndSearch not found");
  auto *SystemDataTy = getFuncArgPtrElementType(IsEndSearch, 0);
  auto *SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, SystemDataTy);
  auto *IsEnd = CrossInliner.inlineCall(Builder, IsEndSearch, SystemData).returnValue;
  Instruction *Then = SplitBlockAndInsertIfThen(IsEnd, &*Builder.GetInsertPoint(), true);
  Builder.SetInsertPoint(Then);

  FunctionEndData EData;
  EData.Terminator = Then;
  processFunctionEnd(Data, EData);
}

/// Replace a call to Await with a call to a given address and pass generated
/// token into an await call
void LowerRaytracingPipelinePassImpl::replaceContinuationCall(ContinuationCallType CallType, CallInst *Call,
                                                              const FunctionData &Data, Value *PayloadOrAttrs,
                                                              Type *PayloadOrAttrsTy) {
  Builder.SetInsertPoint(Call);

  const PAQSerializationLayout *OutgoingSerializationLayout = nullptr;
  const PAQSerializationInfoBase *OutgoingSerializationInfo = nullptr;
  // The number of used payload registers incoming to the resume function of the
  // current continuation call.
  std::optional<uint32_t> ReturnedRegisterCount;
  std::optional<PAQShaderStage> ShaderStage;
  if (CallType != ContinuationCallType::AnyHit) {
    // Specify hit attribute size also in case it is used for CallShader.
    // It is ignored by the implementation in that case.
    PAQPayloadConfig PAQConfig = {PayloadOrAttrsTy, MetadataState.getMaxHitAttributeByteCount()};
    if (CallType == ContinuationCallType::Traversal) {
      const auto *TraceRayInfo = &PAQManager.getOrCreateTraceRaySerializationInfo(PAQConfig);
      OutgoingSerializationInfo = TraceRayInfo;
      OutgoingSerializationLayout = &TraceRayInfo->LayoutsByKind[PAQSerializationLayoutKind::CallerOut];
      ShaderStage = PAQShaderStage::Caller;
      // determine ReturnedRegisterCount
      ReturnedRegisterCount =
          std::min(std::max(TraceRayInfo->LayoutsByKind[PAQSerializationLayoutKind::ClosestHitOut].NumStorageI32s,
                            TraceRayInfo->LayoutsByKind[PAQSerializationLayoutKind::MissOut].NumStorageI32s),
                   MetadataState.getMaxPayloadRegisterCount());

    } else {
      assert(CallType == ContinuationCallType::CallShader && "Unexpected call type!");
      const auto *CallShaderInfo = &PAQManager.getOrCreateCallShaderSerializationInfo(PAQConfig);
      OutgoingSerializationLayout = &CallShaderInfo->CallShaderSerializationLayout;
      OutgoingSerializationInfo = CallShaderInfo;
      // For CallShader, incoming and outgoing layouts are the same
      ReturnedRegisterCount =
          std::min(MetadataState.getMaxPayloadRegisterCount(), OutgoingSerializationLayout->NumStorageI32s);
    }
    assert(OutgoingSerializationLayout && "Missing serialization layout!");
  } else {
    assert(CallType == ContinuationCallType::AnyHit && "Unexpected call type!");
    // For intersection, assume maximum possible number of payload registers.
    ReturnedRegisterCount = MetadataState.getMaxPayloadRegisterCount();
  }

  if (OutgoingSerializationLayout) {
    // Set up the payload spill pointer if necessary
    if (OutgoingSerializationLayout->PayloadMemPointerNode) {
      assert(Data.PayloadSpillSize != 0 && "Inconsistent payload stack size");

      Value *LocalPayloadMem = Builder.CreatePtrToInt(Data.SpilledPayload, I32);
#ifndef NDEBUG
      // Check that payload pointer exists and is in first position
      auto It = OutgoingSerializationLayout->NodeStorageInfos.find(OutgoingSerializationLayout->PayloadMemPointerNode);
      assert(It != OutgoingSerializationLayout->NodeStorageInfos.end() &&
             (It->second.IndexIntervals ==
              PAQIndexIntervals{{FirstPayloadMemoryPointerRegister, FirstPayloadMemoryPointerRegister + 1}}) &&
             "Payload memory pointer at unexpected location!");
#endif

      // Copy to payload storage
      Value *CastPayload = Builder.CreateBitCast(
          Data.PayloadStorage, I32->getPointerTo(Data.PayloadStorage->getType()->getPointerAddressSpace()));

      Builder.CreateStore(LocalPayloadMem, CastPayload);
      // Set stacksize metadata on F
      setStacksizeMetadata(*Call->getFunction(), Data.PayloadSpillSize);
    }
    // Copy local payload to global payload, before await call (e.g. TraceRay,
    // CallShader)
    copyPayload(*PayloadOrAttrsTy, PayloadOrAttrs, Data.PayloadStorage, ShaderStage, PAQAccessKind::Write,
                *OutgoingSerializationLayout);
  }

  auto *ShaderAddr = Call->getArgOperand(0);

  auto *FTy = Call->getFunctionType();
  SmallVector<Type *, 2> ArgTys;
  SmallVector<Value *, 2> Args;

  bool IsWait = (Call->getCalledFunction()->getName().starts_with("_AmdWaitAwait"));

  Value *WaitMask = nullptr;
  Value *RetAddr = nullptr;
  if (MetadataState.isInLgcCpsMode()) {
    // For LgcCps, skip function-addr, the return address will be filled at late
    // stage of continuation transform. Add shader index so that the callee cps
    // function get correct shader-index being passed in.

    // Append the wait mask to the begin of the tail args.
    if (IsWait) {
      constexpr static uint32_t WaitMaskIdx = 1;
      ArgTys.push_back(FTy->getParamType(WaitMaskIdx));
      Args.push_back(Call->getArgOperand(WaitMaskIdx));
    }

    ArgTys.push_back(I32);
    auto *ShaderIndex = CrossInliner
                            .inlineCall(Builder, GetLocalRootIndex,
                                        getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy,
                                                          getFuncArgPtrElementType(GetLocalRootIndex, 0)))
                            .returnValue;
    Args.push_back(ShaderIndex);

    ArgTys.append(FTy->param_begin() + 2, FTy->param_end());
    Args.append(Call->arg_begin() + 2, Call->arg_end());
  } else {
    // We want to avoid having the return address included in the padding
    // computation, since it is included nowhere else. This allows us to compute
    // padding only on the actual tail arguments, which is the only varying part
    // of the final continue call at the end. WaitAwaitTraversal calls don't
    // have a return address, so keep that in mind here.

    if (IsWait)
      WaitMask = Call->getArgOperand(1);

    uint32_t RetAddrArgIndex = IsWait ? 2 : 1;
    if (CallType == ContinuationCallType::Traversal) {
      RetAddr = PoisonValue::get(Builder.getInt64Ty());
    } else {
      RetAddr = Call->getArgOperand(RetAddrArgIndex);
      ++RetAddrArgIndex;
    }

    ArgTys.append(FTy->param_begin() + RetAddrArgIndex, FTy->param_end());
    Args.append(Call->arg_begin() + RetAddrArgIndex, Call->arg_end());
  }

  if (CallType == ContinuationCallType::AnyHit) {
    // Add hit attributes to arguments
    ArgTys.push_back(PayloadOrAttrsTy);
    auto *HitAttrs = Builder.CreateLoad(PayloadOrAttrsTy, PayloadOrAttrs);
    Args.push_back(HitAttrs);
  }

  Instruction *Annotatable = nullptr;
  Value *NewCall = nullptr;

  uint32_t OutgoingPayloadDwords = 0;
  if (Data.NumPassedThroughPayloadDwords.has_value()) {
    OutgoingPayloadDwords = Data.NumPassedThroughPayloadDwords.value();
  } else {
    OutgoingPayloadDwords = std::min(OutgoingSerializationLayout ? OutgoingSerializationLayout->NumStorageI32s
                                                                 : MetadataState.getMaxPayloadRegisterCount(),
                                     MetadataState.getMaxPayloadRegisterCount());
  }

  SmallVector<Type *> ReturnedArgTys{Call->getType()};

  const bool IsLgcCpsMode = MetadataState.isInLgcCpsMode();
  const bool HasPayload = Data.FirstPayloadArgumentDword.has_value();

  // Add padding so that returned payload starts at a fixed dword.
  // NOTE: In lgc.cps mode, subtract 1 as return address is not
  // included in the returned argument list.
  if (HasPayload) {
    const uint32_t PaddingOffset = IsLgcCpsMode ? 1 : 0;
    const auto &[OutgoingPaddingTy, OutgoingPayloadTy] = PayloadHelper.computePaddingAndPayloadArgTys(
        ArgTys, OutgoingPayloadDwords, Data.FirstPayloadArgumentDword, PaddingOffset);
    Args.push_back(PoisonValue::get(OutgoingPaddingTy));
    Args.push_back(Builder.CreateLoad(OutgoingPayloadTy, Data.PayloadStorage));
  }

  if (IsLgcCpsMode) {
    if (HasPayload) {
      // Compute padding for the resume function so that payload starts at a
      // fixed dword. NOTE: Minus 2 as in lgc.cps mode, return address (i32) and
      // shader index (i32) are not included.
      PayloadHelper.computePaddingAndPayloadArgTys(ReturnedArgTys, ReturnedRegisterCount.value(),
                                                   Data.FirstPayloadArgumentDword, 2);
    }

    auto *NewRetTy = StructType::get(Builder.getContext(), ReturnedArgTys);

    Annotatable = insertCpsAwait(NewRetTy, ShaderAddr, Call, Args, CallType, Data.Kind);

    NewCall = Annotatable;
  } else {
    // The wait mask isn't part of regular arguments and thus shouldn't be
    // considered for padding. Thus, we first compute padding, and then add the
    // wait mask.

    // Patch the return address into the await call, since it got excluded for
    // the padding computation previously. For WaitAwaitTraversal, this needs to
    // be removed later once we have the TraversalEntry function.
    ArgTys.insert(ArgTys.begin(), RetAddr->getType());
    Args.insert(Args.begin(), RetAddr);

    if (WaitMask) {
      ArgTys.insert(ArgTys.begin(), WaitMask->getType());
      Args.insert(Args.begin(), WaitMask);
    }

    auto *ShaderTy = FunctionType::get(TokenTy, ArgTys, false);
    auto *ShaderFun = Builder.CreateIntToPtr(ShaderAddr, ShaderTy->getPointerTo());

    auto *Token = Builder.CreateCall(ShaderTy, ShaderFun, Args);

    if (HasPayload) {
      PayloadHelper.computePaddingAndPayloadArgTys(ReturnedArgTys, ReturnedRegisterCount.value(),
                                                   Data.FirstPayloadArgumentDword);
    }

    auto *NewRetTy = StructType::get(Builder.getContext(), ReturnedArgTys);
    auto *Await = getContinuationAwait(*Mod, TokenTy, NewRetTy);
    NewCall = Builder.CreateCall(Await, {Token});
    Annotatable = Token;
  }

  // Copy back returned payload to the payload serialization alloca as part of
  // the payload copying.
  if (HasPayload)
    Builder.CreateStore(Builder.CreateExtractValue(NewCall, ReturnedArgTys.size() - 1), Data.PayloadStorage);

  // For WaitAwait, add metadata indicating that we wait. After coroutine
  // passes, we then generate a waitContinue on the awaited function.
  if (IsWait)
    ContHelper::setIsWaitAwaitCall(*cast<CallInst>(Annotatable));

  ContHelper::ReturnedRegisterCount::setValue(Annotatable, ReturnedRegisterCount.value());

  auto OutgoingRegisterCount = std::min(OutgoingSerializationLayout ? OutgoingSerializationLayout->NumStorageI32s
                                                                    : MetadataState.getMaxPayloadRegisterCount(),
                                        MetadataState.getMaxPayloadRegisterCount());
  // Annotate call with the number of registers used for payload
  ContHelper::OutgoingRegisterCount::setValue(Annotatable, OutgoingRegisterCount);
  if (OutgoingSerializationLayout) {
    MetadataState.updateMaxUsedPayloadRegisterCount(OutgoingRegisterCount);
    MetadataState.updateMaxUsedPayloadRegisterCount(ReturnedRegisterCount.value());
  }

  if (CallType != ContinuationCallType::AnyHit) {
    // Copy global payload back to local payload
    // Overwrite the local payload with poison first, to make sure it is not
    // seen as live state.
    Builder.CreateStore(PoisonValue::get(PayloadOrAttrsTy), PayloadOrAttrs);

    if (CallType == ContinuationCallType::CallShader) {
      // For CallShader, there is only a single layout
      // Copy global payload to local payload, after CallShader call
      copyPayload(*PayloadOrAttrsTy, PayloadOrAttrs, Data.PayloadStorage, ShaderStage, PAQAccessKind::Read,
                  *OutgoingSerializationLayout);
    } else {
      copyTraceRayPayloadIncomingToCaller(*cast<PAQTraceRaySerializationInfo>(OutgoingSerializationInfo),
                                          PayloadOrAttrs, Data.PayloadStorage);
    }
  }

  if (!Call->getType()->isVoidTy()) {
    // Extract the system data from the { %systemData, %padding, %payload }
    // struct returned by the await call.
    NewCall = Builder.CreateExtractValue(NewCall, 0);
    Call->replaceAllUsesWith(NewCall);
  }

  Call->eraseFromParent();
}

/// Replace a call to lgc.rt.shader.index with the passed shader index argument
/// for LgcCps mode or get the value from system data for non-LgcCps mode.
void LowerRaytracingPipelinePassImpl::replaceShaderIndexCall(FunctionData &Data, CallInst *Call) {
  if (Data.Kind == RayTracingShaderStage::RayGeneration) {
    Call->replaceAllUsesWith(Builder.getInt32(0));
  } else {
    Value *ShaderIndex = nullptr;
    if (MetadataState.isInLgcCpsMode()) {
      ShaderIndex = Call->getFunction()->getArg(CpsArgIdxShaderIndex);
    } else {
      assert(Data.SystemDataFirstStore != nullptr);
      Builder.SetInsertPoint(&*++Data.SystemDataFirstStore->getIterator());
      ShaderIndex = CrossInliner
                        .inlineCall(Builder, GetLocalRootIndex,
                                    getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy,
                                                      getFuncArgPtrElementType(GetLocalRootIndex, 0)))
                        .returnValue;
    }
    Call->replaceAllUsesWith(ShaderIndex);
  }
  Call->eraseFromParent();
}

/// Replace a call to lgc.rt.shader.record.buffer with loading the resource.
void LowerRaytracingPipelinePassImpl::replaceShaderRecordBufferCall(FunctionData &Data, CallInst *Call) {
  auto *ShaderRecordBuffer = cast<ShaderRecordBufferOp>(Call);
  auto *TableIndex = ShaderRecordBuffer->getShaderIndex();

  assert(GetSbtAddress && "Could not find GetSbtAddress function");
  assert(GetSbtStride && "Could not find GetSbtStride function");

  Value *TableAddr = CrossInliner.inlineCall(Builder, GetSbtAddress).returnValue;
  Value *TableStride = CrossInliner.inlineCall(Builder, GetSbtStride).returnValue;

  // SBT starts with shader group handle (aka shader identifier), which is 32
  // bytes, then the data for shader record buffer.
  constexpr unsigned ShaderIdEntrySizeInBytes = 32;
  Value *ShaderIdsSizeVal = Builder.getInt32(ShaderIdEntrySizeInBytes);

  // Byte offset = (tableStride * tableIndex) + shaderIdsSize
  Value *Offset = Builder.CreateMul(TableIndex, TableStride);
  Offset = Builder.CreateAdd(Offset, ShaderIdsSizeVal);

  // Zero-extend offset value to 64 bit
  Offset = Builder.CreateZExt(Offset, Builder.getInt64Ty());

  // Final addr
  TableAddr = Builder.CreateAdd(TableAddr, Offset);

  Type *GpuAddrAsPtrTy = PointerType::get(Builder.getContext(), 1 /* ADDR_SPACE_GLOBAL */);
  TableAddr = Builder.CreateIntToPtr(TableAddr, GpuAddrAsPtrTy);

  Call->replaceAllUsesWith(TableAddr);
  Call->eraseFromParent();
}

void LowerRaytracingPipelinePassImpl::handleGetShaderKind(Function &Func) {
  assert(Func.getReturnType()->isIntegerTy(32) && Func.arg_size() == 0);

  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    Function *F = CInst.getFunction();
    auto Stage = getLgcRtShaderStage(F);

    // Ignore GetShaderKind calls where we cannot find the shader kind.
    // This happens e.g. in gpurt-implemented intrinsics that got inlined,
    // but not removed.
    if (!Stage)
      return;

    DXILShaderKind ShaderKind = ShaderStageHelper::rtShaderStageToDxilShaderKind(*Stage);
    auto *ShaderKindVal = ConstantInt::get(Func.getReturnType(), static_cast<uint64_t>(ShaderKind));
    CInst.replaceAllUsesWith(ShaderKindVal);
    CInst.eraseFromParent();
  });
}

void LowerRaytracingPipelinePassImpl::handleGetCurrentFuncAddr(Function &Func) {
  assert(Func.empty() &&
         // Returns an i32 or i64
         (Func.getReturnType()->isIntegerTy(32) || Func.getReturnType()->isIntegerTy(64)));

  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    auto *F = CInst.getFunction();
    auto *RetTy = MetadataState.isInLgcCpsMode() ? Builder.getInt32Ty() : Builder.getInt64Ty();
    Builder.SetInsertPoint(&CInst);
    Value *AsContRef = Builder.create<AsContinuationReferenceOp>(RetTy, F);
    AsContRef = MetadataState.isInLgcCpsMode() ? Builder.CreateZExt(AsContRef, Builder.getInt64Ty()) : AsContRef;
    CInst.replaceAllUsesWith(AsContRef);
    CInst.eraseFromParent();
  });
}

void llvm::copyBytes(IRBuilder<> &B, Value *Dst, Value *Src, uint64_t NumBytes) {
  assert(Dst->getType()->isPointerTy() && Src->getType()->isPointerTy() && "Dst and Src must be pointers!");
  auto *I32 = B.getInt32Ty();

  uint64_t NumFullI32s = NumBytes / RegisterBytes;
  // Copy full I32s
  for (uint64_t I32Index = 0; I32Index < NumFullI32s; ++I32Index) {
    auto *DstPtr = SimplifyingCreateConstInBoundsGEP1_32(B, I32, Dst, I32Index);
    auto *SrcPtr = SimplifyingCreateConstInBoundsGEP1_32(B, I32, Src, I32Index);
    auto *Val = B.CreateLoad(I32, SrcPtr);
    B.CreateStore(Val, DstPtr);
  }

  // Copy remaining bytes
  uint64_t NumRemainingBytes = NumBytes % RegisterBytes;
  if (NumRemainingBytes == 0)
    return;

  // Create i8 loads and stores for the remaining bytes
  Type *I8 = B.getIntNTy(8);
  for (uint64_t I8Index = NumFullI32s * RegisterBytes; I8Index < NumBytes; ++I8Index) {
    auto *DstPtr = SimplifyingCreateConstGEP1_32(B, I8, Dst, I8Index);
    auto *SrcPtr = SimplifyingCreateConstGEP1_32(B, I8, Src, I8Index);
    auto *Val = B.CreateLoad(I8, SrcPtr);
    B.CreateStore(Val, DstPtr);
  }
}

void LowerRaytracingPipelinePassImpl::copyPayload(Type &PayloadTy, Value *LocalPayload, Value *PayloadStorage,
                                                  std::optional<PAQShaderStage> Stage, PAQAccessKind GlobalAccessKind,
                                                  const PAQSerializationLayout &Layout,
                                                  SmallDenseSet<const PAQNode *, 16> *CopiedNodes) {
  // Nothing to do if there is no serialization type, i.e. the layout is empty
  if (!Layout.SerializationTy)
    return;

  LLVM_DEBUG(dbgs() << (GlobalAccessKind == PAQAccessKind::Read ? "Incoming" : "Outgoing")
                    << " serialization layout of " << cast<Instruction>(LocalPayload)->getFunction()->getName() << ": "
                    << *Layout.SerializationTy << "\n");

  Value *SpilledPayloadPtr = nullptr;
  if (Layout.PayloadMemPointerNode) {
    auto *SpillPtr = SimplifyingCreateConstInBoundsGEP1_32(Builder, Builder.getInt8Ty(), PayloadStorage,
                                                           FirstPayloadMemoryPointerRegister);
    SpilledPayloadPtr = Builder.CreateLoad(Builder.getPtrTy(lgc::cps::stackAddrSpace), SpillPtr);
  }

  PayloadCopyHelper Helper{
      *Mod,
      Builder,
      PayloadTy,
      LocalPayload,
      Stage,
      GlobalAccessKind,
      {Builder.getInt32(0)},
      CopiedNodes,
      PayloadStorage,
      &Layout,
      SpilledPayloadPtr,
      MetadataState.getMaxPayloadRegisterCount(),
  };
  Helper.copyPayloadRecursively(Layout.PayloadRootNode);
}

void LowerRaytracingPipelinePassImpl::copyTraceRayPayloadIncomingToCaller(
    const PAQTraceRaySerializationInfo &SerializationInfo, Value *LocalPayload, Value *PayloadStorage) {
  SmallDenseSet<const PAQNode *, 16> CopiedNodes;

  for (auto LayoutKind : {PAQSerializationLayoutKind::ClosestHitOut, PAQSerializationLayoutKind::MissOut}) {
    const PAQSerializationLayout &Layout = SerializationInfo.LayoutsByKind[LayoutKind];
    copyPayload(*SerializationInfo.PayloadRootNode->Ty, LocalPayload, PayloadStorage, PAQShaderStage::Caller,
                PAQAccessKind::Read, Layout, &CopiedNodes);
  }
}

void LowerRaytracingPipelinePassImpl::savePayloadRegistersBeforeRecursion(
    Value *PayloadStorage, RayTracingShaderStage Kind, const PAQSerializationLayout &IncomingLayout,
    const PAQSerializationLayout &OutgoingLayout, SmallVectorImpl<Value *> &SavedRegisterValues) {

  if (!OutgoingLayout.SerializationTy)
    return;

  SavedRegisterValues.resize(MetadataState.getMaxPayloadRegisterCount());

  std::optional<PAQShaderStage> Stage = rtShaderStageToPAQShaderStage(Kind);
  auto *RegTy = Builder.getIntNTy(RegisterBytes * 8);

  for (const auto &NodeWithStorageInfo : OutgoingLayout.NodeStorageInfos) {
    const PAQNode *Node = NodeWithStorageInfo.first;
    const auto &StorageInfo = NodeWithStorageInfo.second;

    // Memory pointer needs to be handled separately because
    // for callable shaders, Stage is not set.
    // Note that callable shaders always write all fields,
    // so we only need to save the pointer for callables.
    if (Node != OutgoingLayout.PayloadMemPointerNode &&
        (!Stage || Node->AccessMask.value().get(*Stage, PAQAccessKind::Write)))
      continue;

    // A node that is not written should be live in the incoming layout.
    assert(IncomingLayout.NodeStorageInfos.count(Node) && "Unexpectedly dead node!");

    for (const PAQIndexInterval &Interval : StorageInfo.IndexIntervals) {
      for (unsigned I = Interval.Begin; I < std::min(Interval.End, MetadataState.getMaxPayloadRegisterCount()); ++I) {
        // Create backup of the I-th payload register
        auto *LoadPtr = SimplifyingCreateConstGEP1_32(Builder, I32, PayloadStorage, I);
        auto *OldValue = Builder.CreateLoad(RegTy, LoadPtr);
        // As long as we keep a 32 bit alignment of all fields, all fields
        // get disjoint registers, and we should never save a register twice.
        // In case we change that in the future, this assertion will fail,
        // in which case we can just avoid duplicate saving.
        // Until now, keep the assert to check our assumptions about
        // the struct layouts.
        assert(I < SavedRegisterValues.size() && "Invalid index!");
        assert(SavedRegisterValues[I] == nullptr && "Duplicate saved value!");
        SavedRegisterValues[I] = OldValue;
      }
    }
  }

  assert((OutgoingLayout.PayloadMemPointerNode == nullptr || SavedRegisterValues[FirstPayloadMemoryPointerRegister]) &&
         "Payload mem pointer missing from saved registers!");
}

void LowerRaytracingPipelinePassImpl::restorePayloadRegistersAfterRecursion(
    Value *PayloadStorage, const SmallVectorImpl<Value *> &SavedRegisterValues) {
  for (unsigned I = 0; I < SavedRegisterValues.size(); ++I) {
    Value *OldValue = SavedRegisterValues[I];
    if (OldValue) {
      auto *StorePtr = SimplifyingCreateConstGEP1_32(Builder, I32, PayloadStorage, I);
      Builder.CreateStore(SavedRegisterValues[I], StorePtr);
    }
  }
}

void LowerRaytracingPipelinePassImpl::copyHitAttributes(FunctionData &Data, Value *SystemDataPtr, Type *SystemDataPtrTy,
                                                        Value *LocalHitAttributes, bool GlobalToLocal,
                                                        const PAQSerializationLayout *Layout) {
  auto *InlineHitAttrsTy = GetTriangleHitAttributes->getReturnType();
  uint64_t InlineHitAttrsBytes = getInlineHitAttrsBytes(*GpurtLibrary);
  uint64_t InlineRegSize = InlineHitAttrsBytes / RegisterBytes;
  auto *RegTy = Builder.getIntNTy(RegisterBytes * 8);

  // Hit attribute storage is split between inline hit attributes in system
  // data, and possibly some payload registers. In order to access inline hit
  // attributes in the same way as payload registers (modeled as global i32
  // array), we add an alloca for inline hit attributes, copy from system data
  // to the alloca at the start, or copy back from the alloca to system data,
  // depending on GlobalToLocal. Then, in the actual copy implementation, we
  // just access the alloca using loads and stores as for payload registers.
  auto InsertPoint = Builder.saveIP();
  Builder.SetInsertPoint(Builder.GetInsertBlock()->getParent()->getEntryBlock().getFirstNonPHI());
  auto *InlineHitAttrsAlloc = Builder.CreateAlloca(InlineHitAttrsTy);
  auto *RegTyPtr = RegTy->getPointerTo(InlineHitAttrsAlloc->getAddressSpace());
  Builder.restoreIP(InsertPoint);
  auto *InlineHitAttrs = Builder.CreateBitCast(InlineHitAttrsAlloc, RegTyPtr);

  if (GlobalToLocal) {
    // Load inline hit attributes from system data
    auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(GetTriangleHitAttributes, 0));
    auto *SystemData = getDXILSystemData(Builder, SystemDataPtr, SystemDataPtrTy, SystemDataTy);
    auto *InlineHitAttrs = CrossInliner.inlineCall(Builder, GetTriangleHitAttributes, SystemData).returnValue;
    Builder.CreateStore(InlineHitAttrs, InlineHitAttrsAlloc);
  }

  // Hit attribute storage in payload storage
  Value *PayloadHitAttrs = nullptr;
  [[maybe_unused]] unsigned PayloadHitAttrBytes = 0;

  // Find hit attributes in layout if present
  if (Layout) {
    if (Layout->HitAttributeStorageNode) {
      auto It = Layout->NodeStorageInfos.find(Layout->HitAttributeStorageNode);
      assert(It != Layout->NodeStorageInfos.end() && "Missing hit attributes in layout!");
      const PAQIndexIntervals &IndexIntervals = It->second.IndexIntervals;
      assert(IndexIntervals.size() == 1 && "Hit attributes must be contiguous!");
      const PAQIndexInterval &IndexInterval = IndexIntervals[0];

      // Obtain pointer to global payload serialization struct
      Value *PayloadSerialization = Builder.CreateBitCast(
          Data.PayloadStorage,
          Layout->SerializationTy->getPointerTo(Data.PayloadStorage->getType()->getPointerAddressSpace()));
      // Last zero yields pointer to the first element of the i32 array
      PayloadHitAttrs =
          Builder.CreateInBoundsGEP(Layout->SerializationTy, PayloadSerialization,
                                    {Builder.getInt32(0), Builder.getInt32(0), Builder.getInt32(IndexInterval.Begin)});
      PayloadHitAttrBytes = RegisterBytes * IndexInterval.size();
    } else {
      // Inline attributes suffice, nothing to do.
    }
  } else {
    assert(Data.Kind == RayTracingShaderStage::Intersection && "Unexpected shader kind");
    // We are in an intersection shader, which does not know the payload type.
    // Assume maximum possible size
    PayloadHitAttrBytes = MetadataState.getMaxHitAttributeByteCount() - InlineHitAttrsBytes;
    // Use hit attribute storage at fixed index
    PayloadHitAttrs =
        SimplifyingCreateConstGEP1_32(Builder, I32, Data.PayloadStorage, FirstPayloadHitAttributeStorageRegister);
  }

  uint64_t HitAttrsBytes = DL->getTypeStoreSize(Data.HitAttributes).getFixedValue();
  if (HitAttrsBytes > MetadataState.getMaxHitAttributeByteCount())
    report_fatal_error("Hit attributes are too large!");
  assert(InlineHitAttrsBytes + PayloadHitAttrBytes >= HitAttrsBytes && "Insufficient hit attribute storage!");
  LocalHitAttributes = Builder.CreateBitCast(LocalHitAttributes, RegTyPtr);
  auto *I8Ty = Builder.getInt8Ty();
  for (unsigned I = 0; I < divideCeil(HitAttrsBytes, RegisterBytes); I++) {
    auto *LocalPtr = SimplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, LocalHitAttributes, I);
    Value *GlobalPtr;
    if (I < InlineRegSize)
      GlobalPtr = SimplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, InlineHitAttrs, I);
    else
      GlobalPtr = SimplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, PayloadHitAttrs, I - InlineRegSize);

    auto *LoadPtr = GlobalToLocal ? GlobalPtr : LocalPtr;
    auto *StorePtr = GlobalToLocal ? LocalPtr : GlobalPtr;
    if ((I + 1) * RegisterBytes <= HitAttrsBytes) {
      // Can load a whole register
      auto *Val = Builder.CreateLoad(RegTy, LoadPtr);
      Builder.CreateStore(Val, StorePtr);
    } else {
      // Load byte by byte into a vector and pad the rest with undef
      auto *ByteLoadPtr = Builder.CreateBitCast(LoadPtr, I8Ty->getPointerTo());
      auto *ByteStorePtr = Builder.CreateBitCast(StorePtr, I8Ty->getPointerTo());
      for (unsigned J = 0; J < HitAttrsBytes % RegisterBytes; J++) {
        auto *Val = Builder.CreateLoad(I8Ty, SimplifyingCreateConstInBoundsGEP1_32(Builder, I8Ty, ByteLoadPtr, J));
        Builder.CreateStore(Val, SimplifyingCreateConstInBoundsGEP1_32(Builder, I8Ty, ByteStorePtr, J));
      }
    }
  }

  if (!GlobalToLocal) {
    // Store inline hit attributes to system data
    auto *Attrs = Builder.CreateLoad(InlineHitAttrsTy, InlineHitAttrsAlloc);
    auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(GetTriangleHitAttributes, 0));
    auto *SystemData = getDXILSystemData(Builder, SystemDataPtr, SystemDataPtrTy, SystemDataTy);
    assert(SetTriangleHitAttributes && "Could not find SetTriangleHitAttributes function");
    CrossInliner.inlineCall(Builder, SetTriangleHitAttributes, {SystemData, Attrs});
  }
}

void LowerRaytracingPipelinePassImpl::setGpurtEntryRegisterCountMetadata() {
  // Even if PreservedPayloadRegisterCount is set, there may be
  // additional shaders in the current module whose usage is recorded
  // in MaxUsedPayloadRegisterCount, to take the max with it.
  uint32_t MaxRegisterCount =
      std::max(MetadataState.tryGetPreservedPayloadRegisterCount().value_or(MetadataState.getMaxPayloadRegisterCount()),
               MetadataState.getMaxUsedPayloadRegisterCount());

  struct VisitorState {
    ModuleMetadataState &Metadata;
    uint32_t MaxRegisterCount;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .addSet<lgc::ilcps::ContinueOp, lgc::ilcps::WaitContinueOp>([](VisitorState &State, Instruction &Op) {
            uint32_t InRegisterCount = 0;
            uint32_t OutRegisterCount = 0;
            auto *CallerFunc = Op.getFunction();
            auto ShaderStage = getLgcRtShaderStage(CallerFunc);
            if (!ShaderStage)
              return;

            switch (ShaderStage.value()) {
            case RayTracingShaderStage::Traversal:
              InRegisterCount = State.MaxRegisterCount;
              OutRegisterCount = State.MaxRegisterCount;
              break;
            case RayTracingShaderStage::KernelEntry:
              InRegisterCount = 0;
              OutRegisterCount = 0;
              break;
            default:
              return;
            }

            assert(!ContHelper::OutgoingRegisterCount::tryGetValue(&Op).has_value() &&
                   "Unexpected register count metadata");
            ContHelper::OutgoingRegisterCount::setValue(&Op, OutRegisterCount);
            State.Metadata.updateMaxUsedPayloadRegisterCount(OutRegisterCount);

            assert(ContHelper::IncomingRegisterCount::tryGetValue(CallerFunc).value_or(InRegisterCount) ==
                       InRegisterCount &&
                   "Unexpected incoming register count on Traversal");
            ContHelper::IncomingRegisterCount::setValue(CallerFunc, InRegisterCount);
            State.Metadata.updateMaxUsedPayloadRegisterCount(InRegisterCount);
          })
          .build();

  VisitorState State{MetadataState, MaxRegisterCount};
  Visitor.visit(State, *Mod);
}

void LowerRaytracingPipelinePassImpl::processContinuations() {
  TokenTy = StructType::create(*Context, "continuation.token")->getPointerTo();
  I32 = Type::getInt32Ty(*Context);

  for (auto &FuncData : ToProcess) {
    processFunction(FuncData.first, FuncData.second);
  }
}

void LowerRaytracingPipelinePassImpl::processFunctionEntry(FunctionData &Data, Argument *SystemDataArgument) {
  // See also the system data documentation at the top of Continuations.h.
  Data.SystemData = Builder.CreateAlloca(Data.SystemDataTy);
  Data.SystemData->setName("system.data.alloca");

  // Allocate payload spilling space
  if (Data.PayloadSpillSize > 0)
    Data.SpilledPayload = Builder.CreateAlloca(ArrayType::get(I32, divideCeil(Data.PayloadSpillSize, RegisterBytes)),
                                               nullptr, "payload.spill.alloca");

  // Initialize system data by copying the argument
  Data.SystemDataFirstStore = Builder.CreateStore(SystemDataArgument, Data.SystemData);

  // Shader preamble
  // NOTE: Skip Traversal, as it can call its own shader start function in
  // GPURT directly if needed.
  if (Data.Kind != RayTracingShaderStage::Traversal) {
    auto ShaderStart = ShaderStartOverloads[Data.SystemDataTy];
    if (ShaderStart) {
      CrossInliner.inlineCall(Builder, ShaderStart, Data.SystemData);
    } else if (Mod != GpurtLibrary) {
      // Skip for tests that do not intended to test this functionality,
      // otherwise we need to handwrite _cont_ShaderStart for each test which is
      // redundant and unnecessary.
      // But ensure that it is present in production path, otherwise there could
      // be correctness issue.
      report_fatal_error("_cont_ShaderStart function is missing");
    }
  }
}

void LowerRaytracingPipelinePassImpl::processFunctionEnd(FunctionData &Data, FunctionEndData &EData) {
  AnyHitExitKind AHExitKind = AnyHitExitKind::None;
  bool IsAnyHit = Data.Kind == RayTracingShaderStage::AnyHit;

  if (IsAnyHit) {
    // Default to AcceptHit, which is only implicitly represented by
    // the absence of a call to the other intrinsics.
    AHExitKind = AnyHitExitKind::AcceptHit;
    // Search backwards from the terminator to find a call to one of
    // acceptHitAndEndSearch or ignoreHit.
    if (EData.Terminator != EData.Terminator->getParent()->getFirstNonPHI()) {
      auto Before = --EData.Terminator->getIterator();
      if (isa<AcceptHitAndEndSearchOp>(Before))
        AHExitKind = AnyHitExitKind::AcceptHitAndEndSearch;
      else if (isa<IgnoreHitOp>(Before))
        AHExitKind = AnyHitExitKind::IgnoreHit;
    }
  }

  Builder.SetInsertPoint(EData.Terminator);

  auto *PayloadTy = Data.IncomingPayload;
  if (Data.Kind != RayTracingShaderStage::RayGeneration && Data.Kind != RayTracingShaderStage::Intersection &&
      Data.Kind != RayTracingShaderStage::Traversal) {
    assert(PayloadTy && "Missing payload type!");

    if (IsAnyHit) {
      if (AHExitKind == AnyHitExitKind::AcceptHit) {
        // Add a call to AcceptHit
        assert(AcceptHit && "Could not find AcceptHit function");
        auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(AcceptHit, 0));
        auto *SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, SystemDataTy);
        CrossInliner.inlineCall(Builder, AcceptHit, SystemData);
      }

      EData.OutgoingSerializationLayout = &PAQManager.getOrCreateShaderExitSerializationLayout(
          *Data.IncomingPayloadSerializationInfo, Data.Kind, Data.HitAttributes, AHExitKind);
    }
    assert(EData.OutgoingSerializationLayout && "Missing layout");

    // Restore saved registers. This needs to be done *before* copying
    // back the payload, which depends on the restored memory pointer!
    restorePayloadRegistersAfterRecursion(Data.PayloadStorage, EData.SavedRegisterValues);

    // Copy local payload into global payload at end of shader
    if (EData.OutgoingSerializationLayout->NumStorageI32s) {
      copyPayload(*PayloadTy, EData.NewPayload, Data.PayloadStorage, EData.ShaderStage, PAQAccessKind::Write,
                  *EData.OutgoingSerializationLayout);
    }

    if (IsAnyHit) {
      // Copy hit attributes into payload for closest hit
      if (AHExitKind == AnyHitExitKind::AcceptHit || AHExitKind == AnyHitExitKind::AcceptHitAndEndSearch) {
        // TODO Only if there is a ClosestHit shader in any hit group
        // where this AnyHit is used. If there is no ClosestHit, the
        // attributes can never be read, so we don't need to store them.
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy, EData.HitAttrsAlloca, false,
                          EData.OutgoingSerializationLayout);
      } else {
        assert(AHExitKind == AnyHitExitKind::IgnoreHit);
        // Copy original hit attributes
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy, EData.OrigHitAttrsAlloca, false,
                          EData.OutgoingSerializationLayout);
      }
    }
  }

  Value *RetValue = nullptr;
  if (!Data.ReturnTy->isVoidTy()) {
    auto *SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, cast<StructType>(Data.ReturnTy));
    RetValue = Builder.CreateLoad(Data.ReturnTy, SystemData);
  }

  if (Data.Kind == RayTracingShaderStage::RayGeneration) {
    assert(!RetValue && "RayGen cannot return anything");
    if (ExitRayGen)
      handleExitRayGen(Data);

    Builder.CreateRetVoid();
    EData.Terminator->eraseFromParent();

    return;
  }

  const bool IsTraversal = Data.Kind == RayTracingShaderStage::Traversal;
  SmallVector<Value *> PaddingArgs;
  if (MetadataState.isInLgcCpsMode()) {
    // Jump to resume point of caller, pass Poison Rcr and ShaderIndex as they
    // are not meaningful for the case.
    PaddingArgs.append({PoisonValue::get(I32), PoisonValue::get(I32)});
  }

  Function *Parent = EData.Terminator->getFunction();

  SmallVector<Value *> TailArgList;
  unsigned OutgoingRegisterCount = 0;
  // For Traversal and Intersection, only pass through the payload registers
  // after reading them back from the serialization alloca.
  if (Data.NumPassedThroughPayloadDwords.has_value()) {
    OutgoingRegisterCount = Data.NumPassedThroughPayloadDwords.value();
  } else {
    assert(EData.OutgoingSerializationLayout && "LowerRaytracingPipelinePassImpl::processFunctionEnd: No outgoing "
                                                "serialization layout found!");
    OutgoingRegisterCount =
        std::min(EData.OutgoingSerializationLayout->NumStorageI32s, MetadataState.getMaxPayloadRegisterCount());
  }

  Instruction *Ret = nullptr;
  if (MetadataState.isInLgcCpsMode()) {
    if (RetValue)
      PaddingArgs.push_back(RetValue);

    // Construct the tail argument list and append the padding and payload
    // values.
    TailArgList.append(PaddingArgs);
    PayloadHelper.appendPaddingAndPayloadValues(PaddingArgs, TailArgList, OutgoingRegisterCount,
                                                Data.FirstPayloadArgumentDword, Data.PayloadStorage);

    Ret = Builder.create<JumpOp>(Parent->getArg(CpsArgIdxReturnAddr), getPotentialCpsReturnLevels(Data.Kind),
                                 PoisonValue::get(StructType::get(Builder.getContext())), TailArgList);
    Builder.CreateUnreachable();
    EData.Terminator->eraseFromParent();
  } else if (IsTraversal) {
    // TODO: For Traversal, we already have continue calls from the
    // IntrinsicPrepare pass. So, we only want to include padding and payload
    // for these existing calls.
    auto [ContinueCall, ItRange] = PayloadHelper.getContinueCallFromTerminator(EData.Terminator);

    PaddingArgs.append(ItRange.begin(), ItRange.end());
    TailArgList.append(PaddingArgs);

    PayloadHelper.appendPaddingAndPayloadValues(PaddingArgs, TailArgList, OutgoingRegisterCount,
                                                Data.FirstPayloadArgumentDword, Data.PayloadStorage);

    Builder.SetInsertPoint(EData.Terminator);

    // Create a lgc.cps.jump call with all arguments including the padding and the
    // payload.
    Value *ReturnAddr = nullptr;
    Value *WaitMask = nullptr;
    if (auto *WaitContinue = dyn_cast<lgc::ilcps::WaitContinueOp>(ContinueCall)) {
      WaitMask = WaitContinue->getWaitMask();
      ReturnAddr = WaitContinue->getReturnAddr();
    } else if (auto *Continue = dyn_cast<lgc::ilcps::ContinueOp>(ContinueCall)) {
      ReturnAddr = Continue->getReturnAddr();
    }

    assert(ReturnAddr);

    TailArgList.insert(TailArgList.begin(), ReturnAddr);
    CallInst *NewCall = Builder.create<lgc::cps::JumpOp>(
        ContinueCall->getArgOperand(0), -1, PoisonValue::get(StructType::get(ContinueCall->getContext())), TailArgList);

    NewCall->copyMetadata(*ContinueCall);

    if (WaitMask)
      ContHelper::setWaitMask(*NewCall, cast<ConstantInt>(WaitMask)->getZExtValue());

    ContinueCall->eraseFromParent();
  } else {
    if (RetValue)
      PaddingArgs.push_back(RetValue);

    PayloadHelper.appendPaddingAndPayloadValues(PaddingArgs, TailArgList, OutgoingRegisterCount,
                                                Data.FirstPayloadArgumentDword, Data.PayloadStorage);

    // Include the return value (it was already included in the PaddingArgs
    // set itself).
    if (RetValue)
      TailArgList.insert(TailArgList.begin(), RetValue);
    Ret = Builder.create<lgc::ilcps::ReturnOp>(Parent->getArg(0), TailArgList);
    Builder.CreateUnreachable();

    EData.Terminator->eraseFromParent();
  }

  // Annotate the terminator with number of outgoing payload registers.
  // This annotation will be passed along the following transformations,
  // ending up at the final continuation call.
  if (Ret) {
    ContHelper::OutgoingRegisterCount::setValue(Ret, OutgoingRegisterCount);
    if (EData.OutgoingSerializationLayout)
      MetadataState.updateMaxUsedPayloadRegisterCount(OutgoingRegisterCount);
  }
}

void LowerRaytracingPipelinePassImpl::handleExitRayGen(const FunctionData &Data) {
  assert(ExitRayGen && "Could not find ExitRayGen function");
  // Create a call to _cont_ExitRayGen
  auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(ExitRayGen, 0));
  auto *SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, SystemDataTy);
  CrossInliner.inlineCall(Builder, ExitRayGen, SystemData);
}

unsigned LowerRaytracingPipelinePassImpl::getUpperBoundOnTraceRayPayloadRegisters() const {
  unsigned MaxHitAttributeBytes = MetadataState.getMaxHitAttributeByteCount();
  unsigned AttributeBytes =
      MaxHitAttributeBytes - std::min(MaxHitAttributeBytes, unsigned(getInlineHitAttrsBytes(*GpurtLibrary)));
  unsigned PayloadBytes = getMaxPayloadSize(Mod).value_or(MetadataState.getMaxPayloadRegisterCount() * RegisterBytes);

  unsigned IncomingStorageBytes = alignTo(AttributeBytes, RegisterBytes) + alignTo(PayloadBytes, RegisterBytes);
  return std::min(unsigned(divideCeil(IncomingStorageBytes, RegisterBytes)),
                  MetadataState.getMaxPayloadRegisterCount());
}

void LowerRaytracingPipelinePassImpl::processFunction(Function *F, FunctionData &Data) {
  Builder.SetInsertPointPastAllocas(F);

  // Change the return type and arguments for shaders that are not RayGen
  SmallVector<Type *> AllArgTypes;
  Type *NewRetTy;
  Type *SystemDataTy = nullptr;

  uint32_t SystemDataArgumentIndex = 0;

  if (MetadataState.isInLgcCpsMode()) {
    // Create the CPS function header.

    // A CPS function signature consists of:
    //  * State: {}
    //  * Return continuation reference (RCR): i32
    //  * Shader index
    //  * Remaining arguments (system data, optionally hit attributes)
    // We need to determine the starting dword of payload storage in arguments,
    // so that payload starts at a fixed VGPR across all shaders in a pipeline.
    // The overall layout is:
    // | returnAddr | shaderIndex | systemData | hitAttrs | padding | payload |
    // For systemData and hitAttrs, use the max possible sizes for calculation.

    AllArgTypes.push_back(StructType::get(Mod->getContext()));
    AllArgTypes.push_back(Builder.getInt32Ty());
    AllArgTypes.push_back(Builder.getInt32Ty());

    SystemDataArgumentIndex = 3;
  } else {
    // For non-lgc.cps mode, we always have a return address argument, which
    // must not be included in the padding computation. The overall layout is:
    // | returnAddr | systemData | (hitAttrs, remaining args) | padding |
    // payload
    // If we don't pass payload, then for stability reasons, we still pass in a
    // zero- padding and payload-array that remains unused.

    SystemDataArgumentIndex = 1;
  }

  // If the value is not computed in the switch case, it will be re-computed
  // based on the incoming serialization layout info.
  std::optional<unsigned> NumIncomingPayloadDwords;
  switch (Data.Kind) {
  case RayTracingShaderStage::RayGeneration: {
    SystemDataTy = DispatchSystemDataTy;
    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = Builder.getVoidTy();
    NumIncomingPayloadDwords = 0;
    break;
  }
  case RayTracingShaderStage::Intersection: {
    assert(TraversalDataTy && "Failed to detect traversal system data type");
    SystemDataTy = TraversalDataTy;
    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = SystemDataTy;
    Data.NumPassedThroughPayloadDwords = MetadataState.getMaxPayloadRegisterCount();
    break;
  }
  case RayTracingShaderStage::AnyHit: {
    assert(TraversalDataTy && "Failed to detect traversal system data type");
    SystemDataTy = TraversalDataTy;
    AllArgTypes.push_back(SystemDataTy);
    AllArgTypes.push_back(Data.HitAttributes);
    NewRetTy = SystemDataTy;
    break;
  }
  case RayTracingShaderStage::ClosestHit:
  case RayTracingShaderStage::Miss: {
    assert(HitMissDataTy && "Failed to detect hit/miss system data type");
    SystemDataTy = HitMissDataTy;
    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = DispatchSystemDataTy;
    break;
  }
  case RayTracingShaderStage::Callable: {
    SystemDataTy = DispatchSystemDataTy;
    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = SystemDataTy;
    break;
  }
  case RayTracingShaderStage::Traversal: {
    if (MetadataState.isInLgcCpsMode())
      SystemDataTy = getFuncArgPtrElementType(F, 0);
    else
      SystemDataTy = F->getArg(0)->getType();

    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = SystemDataTy;

    // We should have set up preserved register count for Traversal, if not,
    // fall back to max count.
    Data.NumPassedThroughPayloadDwords =
        MetadataState.tryGetPreservedPayloadRegisterCount().value_or(MetadataState.getMaxPayloadRegisterCount());
    break;
  }
  default:
    llvm_unreachable("Unhandled ShaderKind");
  }

  if (!NumIncomingPayloadDwords.has_value()) {
    if (Data.NumPassedThroughPayloadDwords.has_value()) {
      NumIncomingPayloadDwords = Data.NumPassedThroughPayloadDwords.value();
    } else {
      const PAQSerializationLayout &IncomingSerializationLayout = PAQManager.getOrCreateShaderStartSerializationLayout(
          *Data.IncomingPayloadSerializationInfo, Data.Kind, Data.HitAttributes);
      NumIncomingPayloadDwords =
          std::min(IncomingSerializationLayout.NumStorageI32s, MetadataState.getMaxPayloadRegisterCount());
    }
  }

  assert(NumIncomingPayloadDwords.has_value());

  Data.PayloadStorageTy = PayloadHelper.getPayloadStorageTy(MetadataState.getMaxPayloadRegisterCount(), Data);
  Data.FirstPayloadArgumentDword =
      PayloadHelper.getPayloadStartDword(Data, MetadataState.getMaxHitAttributeByteCount(), TraversalDataTy);

  const bool HasPayloadArgument = Data.Kind != RayTracingShaderStage::RayGeneration;
  if (HasPayloadArgument) {
    if (MetadataState.isInLgcCpsMode() && Data.Kind != RayTracingShaderStage::AnyHit) {
      // Add a dummy argument for CpsArgIdxHitAttributes so that the arg index
      // of payload matches CpsArgIdxPayload
      AllArgTypes.push_back(StructType::get(*Context, {}));
    }

    PayloadHelper.computePaddingAndPayloadArgTys(AllArgTypes, NumIncomingPayloadDwords.value(),
                                                 Data.FirstPayloadArgumentDword);
  }

  // Pass in the return address argument
  if (!MetadataState.isInLgcCpsMode())
    AllArgTypes.insert(AllArgTypes.begin(), Builder.getInt64Ty());

  Data.PayloadSpillSize =
      computePayloadSpillSize(Data.MaxOutgoingPayloadI32s, MetadataState.getMaxPayloadRegisterCount());
  assert(Data.PayloadSpillSize == 0 || Data.Kind != RayTracingShaderStage::Intersection);

  auto *FunctionTypeRetTy = MetadataState.isInLgcCpsMode() ? Builder.getVoidTy() : NewRetTy;
  // Create new function to change signature
  auto *NewFuncTy = FunctionType::get(FunctionTypeRetTy, AllArgTypes, false);
  Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ArrayRef<AttributeSet>{});
  NewFunc->takeName(F);
  // FIXME: Remove !pointeetypes metadata to workaround an llvm bug. If struct types
  // are referenced only from metadata, LLVM omits the type declaration when
  // printing IR and fails to read it back in because of an unknown type.
  NewFunc->setMetadata("pointeetys", nullptr);

  llvm::moveFunctionBody(*F, *NewFunc);

  Data.SystemDataTy = cast<StructType>(SystemDataTy);
  processFunctionEntry(Data, NewFunc->getArg(SystemDataArgumentIndex));

  uint64_t RetAddrArgIdx = 0;

  if (MetadataState.isInLgcCpsMode()) {
    NewFunc->getArg(CpsArgIdxContState)->setName("cont.state");
    RetAddrArgIdx = CpsArgIdxReturnAddr;
    NewFunc->getArg(CpsArgIdxShaderIndex)->setName("shader.index");

    // Mark as CPS function with the corresponding level.
    CpsLevel Level = getCpsLevelForShaderStage(Data.Kind);
    setCpsFunctionLevel(*NewFunc, Level);
  }

  if (Data.Kind != RayTracingShaderStage::RayGeneration) {
    if (MetadataState.isInLgcCpsMode()) {
      NewFunc->getArg(CpsArgIdxSystemData)->setName("system.data");
      NewFunc->getArg(CpsArgIdxHitAttributes)->setName("hit.attrs");
    }

    NewFunc->getArg(NewFunc->arg_size() - 2)->setName("padding");
    NewFunc->getArg(NewFunc->arg_size() - 1)->setName("payload");
  }

  Value *NewSystemData = nullptr;
  const bool IsTraversal = Data.Kind == RayTracingShaderStage::Traversal;
  if (IsTraversal && MetadataState.isInLgcCpsMode()) {
    assert(F->arg_size() == 1);
    // System data
    // NOTE: Pointer address space may not match based on data layout, mutate
    // the address space here to keep later GEP valid.
    Data.SystemData->mutateType(
        getWithSamePointeeType(Data.SystemData->getType(), F->getArg(0)->getType()->getPointerAddressSpace()));
    NewSystemData = Data.SystemData;
  } else {
    PayloadHelper.initializePayloadSerializationStorage(NewFunc, Data);

    if (auto *ContPayloadRegistersGetI32 = Mod->getFunction("_AmdContPayloadRegistersGetI32"))
      handleContPayloadRegistersGetI32(*ContPayloadRegistersGetI32, *NewFunc, Data);

    if (auto *ContPayloadRegistersSetI32 = Mod->getFunction("_AmdContPayloadRegistersSetI32"))
      handleContPayloadRegistersSetI32(*ContPayloadRegistersSetI32, *NewFunc, Data);

    if (IsTraversal) {
      // Replace old system data argument with cloned functions' argument
      NewSystemData = NewFunc->getArg(1);
    }
  }

  if (NewSystemData)
    F->getArg(0)->replaceAllUsesWith(NewSystemData);

  NewFunc->getArg(RetAddrArgIdx)->setName("returnAddr");

  FunctionEndData EData;
  if (Data.Kind == RayTracingShaderStage::RayGeneration) {
    if (!MetadataState.isInLgcCpsMode()) {
      NewFunc->setMetadata(ContHelper::MDEntryName, MDTuple::get(*Context, {}));

      // Entry functions have no incoming payload or continuation state
      ContHelper::IncomingRegisterCount::setValue(NewFunc, 0);
    }
  } else {
    // Ignore payload for intersection shaders, they don't touch payload
    Value *NewPayload = nullptr;
    // Hit attributes stored in payload at entry of any hit
    Value *OrigHitAttrsAlloca = nullptr;
    // Hit attributes passed to any hit as argument
    Value *HitAttrsAlloca = nullptr;

    Type *PayloadTy = Data.IncomingPayload;
    std::optional<PAQShaderStage> ShaderStage = rtShaderStageToPAQShaderStage(Data.Kind);
    PAQSerializationInfoBase *SerializationInfo = Data.IncomingPayloadSerializationInfo;

    // Check that our assumptions about the number of required payload registers
    // are correct. We exclude callable shaders because the max payload size
    // doesn't apply to them.
    assert((Data.Kind == RayTracingShaderStage::Callable || SerializationInfo == nullptr ||
            std::min(MetadataState.getMaxPayloadRegisterCount(), SerializationInfo->MaxStorageI32s) <=
                getUpperBoundOnTraceRayPayloadRegisters()) &&
           "Payload serialization layout uses too many registers!");

    // For ClosestHit and Miss, we need to determine the out layout
    // early on in order to determine which payload fields to save in case of
    // recursive TraceRay / CallShader.
    const PAQSerializationLayout *OutgoingSerializationLayout = nullptr;
    // Maps indices of payload registers to the saved values (across a
    // recursive TraceRay or CallShader)
    SmallVector<Value *, 32> SavedRegisterValues{};

    if (Data.Kind != RayTracingShaderStage::Intersection && Data.Kind != RayTracingShaderStage::Traversal) {
      assert(PayloadTy && "Missing payload type!");

      // For AnyHit, the layout depends on whether we accept or ignore, which
      // we do not know yet. In that case, the layout is determined later.
      if (Data.Kind != RayTracingShaderStage::AnyHit) {
        OutgoingSerializationLayout = &PAQManager.getOrCreateShaderExitSerializationLayout(
            *SerializationInfo, Data.Kind, Data.HitAttributes, AnyHitExitKind::None);
      }

      const PAQSerializationLayout &IncomingSerializationLayout =
          PAQManager.getOrCreateShaderStartSerializationLayout(*SerializationInfo, Data.Kind, Data.HitAttributes);
      // Handle reading global payload
      auto *FPayload = F->getArg(0);

      {
        // Preserve current insert point
        IRBuilder<>::InsertPointGuard Guard(Builder);
        Builder.SetInsertPointPastAllocas(NewFunc);
        NewPayload = Builder.CreateAlloca(PayloadTy);
        FPayload->replaceAllUsesWith(NewPayload);
      }

      auto IncomingRegisterCount =
          std::min(IncomingSerializationLayout.NumStorageI32s, MetadataState.getMaxPayloadRegisterCount());
      MetadataState.updateMaxUsedPayloadRegisterCount(IncomingRegisterCount);
      if (!MetadataState.isInLgcCpsMode()) {
        // Annotate function with the number of registers for incoming payload
        ContHelper::IncomingRegisterCount::setValue(NewFunc, IncomingRegisterCount);
      }

      // Copy global payload into local payload at start of shader
      if (IncomingSerializationLayout.NumStorageI32s) {
        copyPayload(*PayloadTy, NewPayload, Data.PayloadStorage, ShaderStage, PAQAccessKind::Read,
                    IncomingSerializationLayout);
      }

      if (!Data.CallShaderCalls.empty() || !Data.TraceRayCalls.empty()) {
        assert(OutgoingSerializationLayout && "Missing outgoing serialization layout!");
        savePayloadRegistersBeforeRecursion(Data.PayloadStorage, Data.Kind, IncomingSerializationLayout,
                                            *OutgoingSerializationLayout, SavedRegisterValues);
      }

      // Handle hit attributes
      if (Data.Kind == RayTracingShaderStage::AnyHit) {
        assert(F->arg_size() == 2 && "Shader has more arguments than expected");
        auto *HitAttrs = F->getArg(1);

        {
          // Preserve current insert point
          IRBuilder<>::InsertPointGuard Guard(Builder);
          Builder.SetInsertPointPastAllocas(NewFunc);
          OrigHitAttrsAlloca =
              Builder.CreateAlloca(ArrayType::get(I32, divideCeil(GlobalMaxHitAttributeBytes, RegisterBytes)));
          OrigHitAttrsAlloca->setName("OrigHitAttrs");

          HitAttrsAlloca = Builder.CreateAlloca(Data.HitAttributes);
          HitAttrsAlloca->setName("HitAttrsAlloca");
        }

        // Copy old hit attributes from payload
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy, OrigHitAttrsAlloca, true,
                          &IncomingSerializationLayout);

        // Copy new hit attributes from argument:
        // Since the argument list of NewFunc ends with padding and payload,
        // subtract 3 to get the hit attributes.
        unsigned HitAttributesIdx = MetadataState.isInLgcCpsMode() ? CpsArgIdxHitAttributes : NewFunc->arg_size() - 3;
        Builder.CreateStore(NewFunc->getArg(HitAttributesIdx), HitAttrsAlloca);
        HitAttrs->replaceAllUsesWith(HitAttrsAlloca);
      } else if (Data.Kind == RayTracingShaderStage::ClosestHit) {
        assert(F->arg_size() == 2 && "Shader has more arguments than expected");
        auto *OrigHitAttrs = F->getArg(1);

        Value *NewHitAttrs;
        {
          // Preserve current insert point
          IRBuilder<>::InsertPointGuard Guard(Builder);
          Builder.SetInsertPointPastAllocas(NewFunc);
          NewHitAttrs = Builder.CreateAlloca(Data.HitAttributes);
          NewHitAttrs->setName("HitAttrs");
        }

        // Copy hit attributes from system data and payload into the local
        // variable
        OrigHitAttrs->replaceAllUsesWith(NewHitAttrs);
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy, NewHitAttrs, true, &IncomingSerializationLayout);
      }
    } else {
      if (!MetadataState.isInLgcCpsMode()) {
        if (Data.Kind == RayTracingShaderStage::Intersection) {
          // Annotate intersection shader with the maximum number of registers
          // used for payload
          // TODO: When compiling a pipeline and not a library, we could figure
          //       out the pipeline-wide max (on a higher level than here) and
          //       use that instead. For a library compile, we can't know the
          //       max payload size of shaders in pipelines this shader is used
          //       in.
          ContHelper::IncomingRegisterCount::setValue(NewFunc, MetadataState.getMaxPayloadRegisterCount());
          // Intentionally do NOT update MaxUsedPayloadRegisterCount
        } else {
          assert(Data.Kind == RayTracingShaderStage::Traversal);
          // Intentionally do nothing for Traversal. We explicitly add Traversal
          // register count metadata elsewhere.
        }
      }
    }

    EData.OutgoingSerializationLayout = OutgoingSerializationLayout;
    EData.SavedRegisterValues = std::move(SavedRegisterValues);
    EData.NewPayload = NewPayload;
    EData.ShaderStage = ShaderStage;
    EData.HitAttrsAlloca = HitAttrsAlloca;
    EData.OrigHitAttrsAlloca = OrigHitAttrsAlloca;
  }
  Data.ReturnTy = NewRetTy;

  // Modify function ends
  // While iterating over function ends, basic blocks are inserted by inlining
  // functions, so we copy them beforehand.
  if (MetadataState.isInLgcCpsMode() && Data.Kind == RayTracingShaderStage::Traversal) {
    PayloadHelper.patchJumpCalls(NewFunc, Data.JumpCalls, Data.FirstPayloadArgumentDword);
  } else {
    SmallVector<BasicBlock *> BBs(make_pointer_range(*NewFunc));
    for (auto *BB : BBs) {
      auto *I = BB->getTerminator();
      assert(I && "BB must have terminator");
      // Replace the end of the BB if it terminates the function
      bool IsFunctionEnd = (I->getOpcode() == Instruction::Ret || I->getOpcode() == Instruction::Unreachable);
      if (IsFunctionEnd) {
        EData.Terminator = I;
        processFunctionEnd(Data, EData);
      }
    }
  }

  // Remove the old function
  F->replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F->getType()));
  F->eraseFromParent();
  F = NewFunc;

  MDTuple *ContMDTuple = MDTuple::get(*Context, {ValueAsMetadata::get(F)});
  F->setMetadata(ContHelper::MDContinuationName, ContMDTuple);

  // Replace TraceRay calls
  for (auto *Call : Data.TraceRayCalls) {
    assert(TraceRay && "TraceRay not found");
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceCall(Data, Call, TraceRay, ContinuationCallType::Traversal);
  }

  // Replace ReportHit calls
  for (auto *Call : Data.ReportHitCalls) {
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceReportHitCall(Data, Call);
  }

  // Replace CallShader calls
  for (auto *Call : Data.CallShaderCalls) {
    assert(CallShader && "CallShader not found");
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceCall(Data, Call, CallShader, ContinuationCallType::CallShader);
  }

  // Replace ShaderIndexOp calls
  for (auto *Call : Data.ShaderIndexCalls)
    replaceShaderIndexCall(Data, Call);

  // Replace ShaderRecordBufferOp calls
  for (auto *Call : Data.ShaderRecordBufferCalls) {
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceShaderRecordBufferCall(Data, Call);
  }

  // Replace non-rematerializable intrinsic calls
  for (auto *Call : Data.IntrinsicCalls)
    replaceIntrinsicCall(Builder, Data.SystemDataTy, Data.SystemData, Data.Kind, Call, GpurtLibrary, CrossInliner);

#ifndef NDEBUG
  if (!MetadataState.isInLgcCpsMode() && Data.Kind != RayTracingShaderStage::RayGeneration) {
    // Check that all returns have registercount metadata
    for (const auto &BB : *F) {
      auto *Terminator = BB.getTerminator();
      if (Terminator->getOpcode() == Instruction::Ret && !ContHelper::OutgoingRegisterCount::tryGetValue(Terminator))
        report_fatal_error("Missing registercount metadata!");
    }
  }
#endif
}

void LowerRaytracingPipelinePassImpl::handleContPayloadRegisterI32Count(Function &F) {
  assert(F.arg_empty()
         // register count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32));

  uint32_t RegCount = ContHelper::MaxUsedPayloadRegisterCount::tryGetValue(Mod).value_or(0);
  auto *RegCountAsConstant = ConstantInt::get(IntegerType::get(F.getContext(), 32), RegCount);

  llvm::replaceCallsToFunction(F, *RegCountAsConstant);
}

void LowerRaytracingPipelinePassImpl::handleContPayloadRegistersGetI32(Function &F, Function &Parent,
                                                                       FunctionData &Data) {
  assert(F.getReturnType()->isIntegerTy(32) &&
         F.arg_size() == 1
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32));

  llvm::forEachCall(F, [&](CallInst &CInst) {
    if (CInst.getFunction() != &Parent)
      return;

    if (Data.FirstPayloadArgumentDword.has_value()) {
      Builder.SetInsertPoint(&CInst);
      auto *Addr =
          Builder.CreateGEP(Data.PayloadStorageTy, Data.PayloadStorage, {Builder.getInt32(0), CInst.getArgOperand(0)});
      auto *Load = Builder.CreateLoad(Builder.getInt32Ty(), Addr);
      CInst.replaceAllUsesWith(Load);
    } else {
      CInst.replaceAllUsesWith(PoisonValue::get(Builder.getInt32Ty()));
    }
    CInst.eraseFromParent();
  });
}

void LowerRaytracingPipelinePassImpl::handleContPayloadRegistersSetI32(Function &F, Function &Parent,
                                                                       FunctionData &Data) {
  assert(F.getReturnType()->isVoidTy() &&
         F.arg_size() == 2
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  llvm::forEachCall(F, [&](CallInst &CInst) {
    if (CInst.getFunction() != &Parent)
      return;

    if (Data.FirstPayloadArgumentDword.has_value()) {
      Builder.SetInsertPoint(&CInst);
      auto *Addr =
          Builder.CreateGEP(Data.PayloadStorageTy, Data.PayloadStorage, {Builder.getInt32(0), CInst.getArgOperand(0)});
      Builder.CreateStore(CInst.getOperand(1), Addr);
    }
    CInst.eraseFromParent();
  });
}

void LowerRaytracingPipelinePassImpl::collectProcessableFunctions() {
  for (auto &Func : *Mod) {
    auto Stage = getLgcRtShaderStage(&Func);
    if (!Stage || Func.isDeclaration())
      continue;

    // Skip kernel entry
    if (Stage == RayTracingShaderStage::KernelEntry)
      continue;

    RayTracingShaderStage Kind = *Stage;
    switch (Kind) {
    case RayTracingShaderStage::RayGeneration:
    case RayTracingShaderStage::Intersection:
    case RayTracingShaderStage::AnyHit:
    case RayTracingShaderStage::ClosestHit:
    case RayTracingShaderStage::Miss:
    case RayTracingShaderStage::Callable:
    case RayTracingShaderStage::Traversal: {
      FunctionData Data;
      Data.Kind = Kind;

      if (Kind != RayTracingShaderStage::Intersection && Kind != RayTracingShaderStage::RayGeneration &&
          Kind != RayTracingShaderStage::Traversal) {
        assert(!Func.arg_empty() && "Shader must have at least one argument");
        Data.IncomingPayload = getFuncArgPtrElementType(&Func, 0);
        PAQPayloadConfig PAQConfig = {Data.IncomingPayload, MetadataState.getMaxHitAttributeByteCount()};
        Data.IncomingPayloadSerializationInfo = &PAQManager.getOrCreateSerializationInfo(PAQConfig, Kind);
        assert(Data.IncomingPayloadSerializationInfo != nullptr && "Missing serialization info!");
      }
      if (Kind == RayTracingShaderStage::AnyHit || Kind == RayTracingShaderStage::ClosestHit) {
        assert(Func.arg_size() >= 2 && "Shader must have at least two arguments");
        Data.HitAttributes = getFuncArgPtrElementType(&Func, Func.arg_size() - 1);
      }

      if (Kind == RayTracingShaderStage::Intersection) {
        Data.MaxOutgoingPayloadI32s = MetadataState.getMaxPayloadRegisterCount();
      }

      ToProcess[&Func] = Data;
      break;
    }
    default:
      break;
    }
  }
}

void LowerRaytracingPipelinePassImpl::handleAmdInternalFunc(Function &Func) {
  StringRef FuncName = Func.getName();

  if (FuncName.starts_with("_AmdRestoreSystemData")) {
    assert(Func.arg_size() == 1
           // Function address
           && Func.getFunctionType()->getParamType(0)->isPointerTy());
    llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
      Builder.SetInsertPoint(&CInst);
      handleRestoreSystemData(&CInst);
    });
  } else if (FuncName.starts_with("_AmdGetFuncAddr")) {
    ContHelper::handleGetFuncAddr(Func, Builder);
  } else if (FuncName.starts_with("_AmdGetShaderKind")) {
    handleGetShaderKind(Func);
  } else if (FuncName.starts_with("_AmdGetCurrentFuncAddr")) {
    handleGetCurrentFuncAddr(Func);
  }
}

// Split BB after _AmdRestoreSystemData.
// The coroutine passes rematerialize to the start of the basic block of a use.
// We split the block so that every rematerialized dxil intrinsic lands after
// the restore call and accesses the restored system data.
// If we did not do that, an intrinsic that is rematerialized to before
// RestoreSystemData is called gets an uninitialized system data struct as
// argument.
void LowerRaytracingPipelinePassImpl::splitRestoreBB() {
  for (auto &F : *Mod) {
    if (F.getName().starts_with("_AmdRestoreSystemData")) {
      llvm::forEachCall(F, [](llvm::CallInst &CInst) {
        auto *Next = &*++CInst.getIterator();
        CInst.eraseFromParent();
        if (!Next->isTerminator())
          SplitBlock(Next->getParent(), Next);
      });
    }
  }
}

// Search for known intrinsics that cannot be rematerialized
void LowerRaytracingPipelinePassImpl::handleUnrematerializableCandidates() {
  for (auto &Func : *Mod) {
    if (!llvm::isLgcRtOp(&Func))
      continue;

    static const llvm_dialects::OpSet NonRematerializableDialectOps =
        llvm_dialects::OpSet::get<TraceRayOp, ReportHitOp, CallCallableShaderOp, ShaderIndexOp, ShaderRecordBufferOp,
                                  JumpOp>();
    if (!NonRematerializableDialectOps.contains(Func)) {
      llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
        auto Data = ToProcess.find(CInst.getFunction());
        if (Data != ToProcess.end()) {
          if (!ContHelper::isRematerializableLgcRtOp(CInst, Data->second.Kind))
            Data->second.IntrinsicCalls.push_back(&CInst);
        }
      });
    }
  }
}

// Collect GPURT functions and do precondition checks on the fly.
void LowerRaytracingPipelinePassImpl::collectGpuRtFunctions() {
  IsEndSearch = GpurtLibrary->getFunction(ContDriverFunc::IsEndSearchName);
  if (IsEndSearch)
    assert(IsEndSearch->getReturnType()->isIntegerTy(1) &&
           IsEndSearch->arg_size() == 1
           // Traversal data
           && IsEndSearch->getFunctionType()->getParamType(0)->isPointerTy());

  GetTriangleHitAttributes = GpurtLibrary->getFunction(ContDriverFunc::GetTriangleHitAttributesName);
  if (GetTriangleHitAttributes)
    assert(GetTriangleHitAttributes->getReturnType()->isStructTy() // BuiltinTriangleIntersectionAttributes
           && GetTriangleHitAttributes->arg_size() == 1
           // System data
           && GetTriangleHitAttributes->getFunctionType()->getParamType(0)->isPointerTy());

  SetTriangleHitAttributes = GpurtLibrary->getFunction(ContDriverFunc::SetTriangleHitAttributesName);
  if (SetTriangleHitAttributes)
    assert(SetTriangleHitAttributes->getReturnType()->isVoidTy() &&
           SetTriangleHitAttributes->arg_size() == 2
           // System data
           && SetTriangleHitAttributes->getFunctionType()->getParamType(0)->isPointerTy()
           // BuiltinTriangleIntersectionAttributes
           && (SetTriangleHitAttributes->getFunctionType()->getParamType(1)->isStructTy() ||
               SetTriangleHitAttributes->getFunctionType()->getParamType(1)->isPointerTy()));

  GetLocalRootIndex = GpurtLibrary->getFunction(ContDriverFunc::GetLocalRootIndexName);

  assert(GetLocalRootIndex && "Could not find GetLocalRootIndex function");
  assert(GetLocalRootIndex->getReturnType()->isIntegerTy(32) &&
         GetLocalRootIndex->arg_size() == 1
         // Dispatch data
         && GetLocalRootIndex->getFunctionType()->getParamType(0)->isPointerTy());

  SetLocalRootIndex = getSetLocalRootIndex(*Mod);

  ExitRayGen = GpurtLibrary->getFunction(ContDriverFunc::ExitRayGenName);
  if (ExitRayGen)
    assert(ExitRayGen->getReturnType()->isVoidTy() && ExitRayGen->arg_size() == 1 &&
           ExitRayGen->getFunctionType()->getParamType(0)->isPointerTy());

  TraceRay = GpurtLibrary->getFunction(ContDriverFunc::TraceRayName);
  if (TraceRay)
    assert(TraceRay->getReturnType()->isVoidTy() &&
           TraceRay->arg_size() == 15
           // Dispatch data
           && TraceRay->getFunctionType()->getParamType(0)->isPointerTy());

  CallShader = GpurtLibrary->getFunction(ContDriverFunc::CallShaderName);
  if (CallShader)
    assert(CallShader->getReturnType()->isVoidTy() &&
           CallShader->arg_size() == 2
           // Dispatch data
           && CallShader->getFunctionType()->getParamType(0)->isPointerTy()
           // Shader id
           && CallShader->getFunctionType()->getParamType(1)->isIntegerTy(32));

  ReportHit = GpurtLibrary->getFunction(ContDriverFunc::ReportHitName);
  if (ReportHit)
    assert(ReportHit->getReturnType()->isIntegerTy(1) &&
           ReportHit->arg_size() == 3
           // Traversal data
           && ReportHit->getFunctionType()->getParamType(0)->isPointerTy());

  AcceptHit = GpurtLibrary->getFunction(ContDriverFunc::AcceptHitName);
  if (AcceptHit)
    assert(AcceptHit->getReturnType()->isVoidTy() &&
           AcceptHit->arg_size() == 1
           // Traversal data
           && AcceptHit->getFunctionType()->getParamType(0)->isPointerTy());

  GetSbtAddress = GpurtLibrary->getFunction(ContDriverFunc::GetSbtAddressName);
  if (GetSbtAddress)
    assert(GetSbtAddress->getReturnType()->isIntegerTy(64) && GetSbtAddress->arg_empty());

  GetSbtStride = GpurtLibrary->getFunction(ContDriverFunc::GetSbtStrideName);
  if (GetSbtStride)
    assert(GetSbtStride->getReturnType()->isIntegerTy(32) && GetSbtStride->arg_empty());

  // _cont_ShaderStart has one overload for each system data type
  llvm::for_each(GpurtLibrary->functions(), [&](Function &F) {
    if (F.getName().starts_with(ContDriverFunc::ShaderStartName)) {
      assert(F.getReturnType()->isVoidTy() &&
             F.arg_size() == 1
             // System data
             && F.getFunctionType()->getParamType(0)->isPointerTy());
      ShaderStartOverloads[getFuncArgPtrElementType(&F, 0)] = &F;
    }
  });
}

LowerRaytracingPipelinePassImpl::LowerRaytracingPipelinePassImpl(llvm::Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Context{&M.getContext()}, DL{&M.getDataLayout()},
      Builder{Mod->getContext()}, MetadataState{*Mod}, PAQManager{Mod, &GpurtLibrary,
                                                                  MetadataState.getMaxPayloadRegisterCount()},
      PayloadHelper{*Mod, *DL, Builder, MetadataState.isInLgcCpsMode()} {
}

PreservedAnalyses LowerRaytracingPipelinePassImpl::run() {
  collectGpuRtFunctions();
  DispatchSystemDataTy = getFuncArgPtrElementType(GetLocalRootIndex, 0);
  assert(DispatchSystemDataTy && "LowerRaytracingPipelinePassImpl::run: Could "
                                 "not derive DispatchSystemData "
                                 "type from GetLocalRootIndex!");

  collectProcessableFunctions();

  struct VisitorState {
    PAQSerializationInfoManager &PAQManager;
    MapVector<Function *, FunctionData> &Processables;
    ModuleMetadataState &Metadata;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByInstruction)
          .addSet<TraceRayOp, CallCallableShaderOp, ReportHitOp, ShaderIndexOp, ShaderRecordBufferOp, JumpOp>(
              [](VisitorState &State, Instruction &Op) {
                auto *CInst = cast<CallInst>(&Op);
                auto Data = State.Processables.find(CInst->getFunction());
                if (Data == State.Processables.end())
                  return;

                if (isa<ShaderIndexOp>(Op)) {
                  Data->second.ShaderIndexCalls.push_back(CInst);
                  return;
                }

                if (isa<ShaderRecordBufferOp>(Op)) {
                  Data->second.ShaderRecordBufferCalls.push_back(CInst);
                  return;
                }

                if (auto *Jump = dyn_cast<JumpOp>(CInst)) {
                  Data->second.JumpCalls.push_back(Jump);
                  return;
                }

                Type *PayloadTy = ContHelper::getPayloadTypeFromMetadata(*CInst);

                if (!isa<ReportHitOp>(Op)) {
                  PAQPayloadConfig PAQPayload = {PayloadTy, State.Metadata.getMaxHitAttributeByteCount()};

                  uint32_t PayloadStorageI32s = 0;
                  if (isa<TraceRayOp>(Op)) {
                    PayloadStorageI32s = State.PAQManager.getMaxPayloadStorageI32sForTraceRayFunc(PAQPayload);

                    Data->second.TraceRayCalls.push_back(CInst);
                  } else if (isa<CallCallableShaderOp>(Op)) {
                    PayloadStorageI32s = State.PAQManager.getMaxPayloadStorageI32sForCallShaderFunc(PAQPayload);

                    Data->second.CallShaderCalls.push_back(CInst);
                  }

                  Data->second.MaxOutgoingPayloadI32s =
                      std::max(Data->second.MaxOutgoingPayloadI32s, PayloadStorageI32s);
                } else {
                  // The converter uses payload type metadata also to indicate hit
                  // attribute types
                  assert((!Data->second.HitAttributes || Data->second.HitAttributes == PayloadTy) &&
                         "Multiple reportHit calls with different hit attributes");
                  Data->second.HitAttributes = PayloadTy;

                  Data->second.ReportHitCalls.push_back(CInst);
                }
              })
          .build();

  VisitorState S{PAQManager, ToProcess, MetadataState};
  Visitor.visit(S, *Mod);

  handleUnrematerializableCandidates();

  // Find the traversal system data type by looking at the argument to
  // ReportHit.
  TraversalDataTy = nullptr;
  if (ReportHit)
    TraversalDataTy = getFuncArgPtrElementType(ReportHit, 0);
  HitMissDataTy = nullptr;
  if (auto *HitKind = GpurtLibrary->getFunction(ContDriverFunc::HitKindName)) {
    HitMissDataTy = getFuncArgPtrElementType(HitKind, 0);
    LLVM_DEBUG(dbgs() << "HitMiss system data from _cont_HitKind: "; HitMissDataTy->dump());
  }

  setGpurtEntryRegisterCountMetadata();

  processContinuations();

  for (auto &Func : *Mod) {
    if (Func.getName().starts_with("_Amd")) {
      handleAmdInternalFunc(Func);
    }
  }

  splitRestoreBB();

  if (Mod == GpurtLibrary) {
    // For tests, remove intrinsic implementations from the module
    for (auto &F : make_early_inc_range(*Mod)) {
      auto Name = F.getName();
      if (Name.starts_with(ContDriverFunc::TraceRayName) || Name.starts_with(ContDriverFunc::CallShaderName) ||
          Name.starts_with(ContDriverFunc::ExitRayGenName) || Name.starts_with(ContDriverFunc::ReportHitName)) {
        F.eraseFromParent();
      }
    }
  }

  // Remove bitcasts and the DXIL Payload Type metadata in one step to save one
  // full iteration over all functions.
  fixupDxilMetadata(*Mod);

  llvm::removeUnusedFunctionDecls(Mod);

  MetadataState.updateModuleMetadata();

  if (auto *ContPayloadRegistersI32Count = Mod->getFunction("_AmdContPayloadRegistersI32Count"))
    handleContPayloadRegisterI32Count(*ContPayloadRegistersI32Count);

  return PreservedAnalyses::none();
}

std::optional<PAQShaderStage> llvm::rtShaderStageToPAQShaderStage(RayTracingShaderStage ShaderKind) {
  switch (ShaderKind) {
  case RayTracingShaderStage::RayGeneration:
    return PAQShaderStage::Caller;
  case RayTracingShaderStage::Intersection:
    // Explicit: PAQ do not apply to Intersection
    return {};
  case RayTracingShaderStage::AnyHit:
    return PAQShaderStage::AnyHit;
  case RayTracingShaderStage::ClosestHit:
    return PAQShaderStage::ClosestHit;
  case RayTracingShaderStage::Miss:
    return PAQShaderStage::Miss;
  case RayTracingShaderStage::Callable:
    // Explicit: PAQ do not apply to Callable
    return {};
  default:
    return {};
  }
} // anonymous namespace

llvm::PreservedAnalyses LowerRaytracingPipelinePass::run(llvm::Module &M,
                                                         llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass lower-raytracing-pipeline\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  auto &GpurtContext = lgc::GpurtContext::get(M.getContext());
  LowerRaytracingPipelinePassImpl Impl(M, GpurtContext.theModule ? *GpurtContext.theModule : M);
  return Impl.run();
}
