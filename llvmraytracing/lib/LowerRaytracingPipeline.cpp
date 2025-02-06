/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "RematSupport.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
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
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>

#define DEBUG_TYPE "lower-raytracing-pipeline"

using namespace llvm;
using namespace lgc::cps;
using namespace lgc::rt;

namespace {

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
      auto *GlobalIntervalI32Ptr =
          CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(B, I32, Serialization, Interval.Begin);
      TmpIntervals.push_back({Interval, GlobalIntervalI32Ptr});
    }
    if (CompleteInterval.End > PayloadRegisterCount) {
      PAQIndexInterval Interval = {std::max(CompleteInterval.Begin, PayloadRegisterCount), CompleteInterval.End};
      // Pointer to start of current interval in global payload
      auto *GlobalIntervalI32Ptr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(
          B, I32, SpilledPayloadPtr, Interval.Begin - PayloadRegisterCount);
      TmpIntervals.push_back({Interval, GlobalIntervalI32Ptr});
    }

    for (auto [Interval, GlobalIntervalI32Ptr] : TmpIntervals) {
      // Obtain i32-based index from byte-offset. We only expect
      // to increase FieldByteOffset by a non-multiple of RegisterBytes
      // in the last iteration, so here it should always be divisible
      unsigned FieldI32Offset = *FieldByteOffset / RegisterBytes;
      assert(*FieldByteOffset == FieldI32Offset * RegisterBytes);
      // I32 pointer into field, offset by FieldI32Offset
      auto *FieldIntervalI32Ptr =
          CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(B, I32, LocalFieldPtr, FieldI32Offset);

      // Determine Src and Dst
      auto *Src = FieldIntervalI32Ptr;
      auto *Dst = GlobalIntervalI32Ptr;
      if (GlobalAccessKind != PAQAccessKind::Write)
        std::swap(Src, Dst);

      unsigned NumCopyBytes = RegisterBytes * Interval.size();

      unsigned FieldNumRemainingBytes = FieldNumBytes - *FieldByteOffset;
      NumCopyBytes = std::min(NumCopyBytes, FieldNumRemainingBytes);

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

  void updateMaxUsedPayloadRegisterCount(uint32_t Count) {
    MaxUsedPayloadRegisterCount = std::max(Count, MaxUsedPayloadRegisterCount);
  }

  uint32_t getMaxUsedPayloadRegisterCount() const { return MaxUsedPayloadRegisterCount; }

  uint32_t getNumPassedThroughPayloadDwords() const {
    if (MaxUsedPayloadRegisterCountWasSet)
      return MaxUsedPayloadRegisterCount;

    return MaxPayloadRegisterCount;
  }

  uint32_t getMaxHitAttributeByteCount() const { return MaxHitAttributeByteCount; }

  void updateModuleMetadata() const;

private:
  Module &Mod;
  /// MaxPayloadRegisterCount is initialized from metadata. If there is none,
  /// use this default instead:
  static constexpr uint32_t DefaultPayloadRegisterCount = 30;
  /// [In]: Maximum allowed number of registers to be used for the payload.
  ///       It is guaranteed that all modules in a pipeline share this value.
  uint32_t MaxPayloadRegisterCount = 0;
  /// [In/Out]: The maximum number of payload registers written or read by any
  ///           shader in the pipeline observed so far.
  ///           This excludes intersection shaders, which just pass through an existing payload.
  ///           If set on an incoming module, we can rely on it being an upper bound
  ///           for driver functions, because driver functions are compiled last and not
  ///           reused for child pipelines.
  ///           We can't rely on it when compiling app shaders (e.g. intersection).
  uint32_t MaxUsedPayloadRegisterCount = 0;
  /// [In]: The maximum size of hit attribute stored on the module as metadata.
  uint32_t MaxHitAttributeByteCount = 0;
  /// [In]: The address space used for the continuations stack.
  ///       Either stack or global memory.
  ContStackAddrspace StackAddrspace = ContHelper::DefaultStackAddrspace;

  // Describes whether a value for maxUsedPayloadRegisterCount was set in the input module.
  // If that is the case, for driver functions we rely on it.
  // This mechanism ensures we don't rely on it in case the value was only initialized
  // during processing of the current module.
  bool MaxUsedPayloadRegisterCountWasSet = false;
};

class LowerRaytracingPipelinePassImpl final {
public:
  LowerRaytracingPipelinePassImpl(Module &M, Module &GpurtLibrary);
  PreservedAnalyses run();

private:
  struct FunctionData {
    RayTracingShaderStage Kind = RayTracingShaderStage::Count;

#define DECLARE_KIND_GETTER(Stage)                                                                                     \
  bool is##Stage() const { return Kind == RayTracingShaderStage::Stage; }
    DECLARE_KIND_GETTER(RayGeneration)
    DECLARE_KIND_GETTER(Intersection)
    DECLARE_KIND_GETTER(AnyHit)
    DECLARE_KIND_GETTER(ClosestHit)
    DECLARE_KIND_GETTER(Miss)
    DECLARE_KIND_GETTER(Callable)
    DECLARE_KIND_GETTER(Traversal)
    DECLARE_KIND_GETTER(KernelEntry)
#undef DECLARE_KIND_GETTER

    SmallVector<CallInst *> TraceRayCalls;
    SmallVector<CallInst *> ReportHitCalls;
    SmallVector<CallInst *> CallShaderCalls;
    /// Calls to hlsl intrinsics that cannot be rematerialized
    SmallVector<CallInst *> IntrinsicCalls;
    SmallVector<CallInst *> ShaderRecordBufferCalls;
    SmallVector<JumpOp *> JumpCalls;

    /// In any-hit shaders, map known return instructions to their exit kind
    /// for delayed hit attribute processing.
    DenseMap<ReturnInst *, AnyHitExitKind> AnyHitExits;

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
    ReturnInst *Terminator = nullptr;
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
    PayloadHelper(Module &Mod, const DataLayout &DL, llvm_dialects::Builder &Builder)
        : Mod{Mod}, DL{DL}, Builder{Builder} {}

    /// Append padding and payload to lgc.cps.jump calls.
    void patchJumpCalls(Function *Parent, ArrayRef<JumpOp *> JumpCalls, std::optional<uint32_t> PayloadStartDword,
                        std::optional<uint32_t> NumPreservedPayloadDwords,
                        Value *PayloadSerializationStorage = nullptr) {
      if (!PayloadStartDword.has_value())
        return;

      assert(NumPreservedPayloadDwords.has_value() &&
             "PayloadHelper::patchJumpCalls: Expected the number of preserved payload dwords to be set!");
      const uint32_t PayloadSize = NumPreservedPayloadDwords.value();

      for (auto *Jump : JumpCalls) {
        Builder.SetInsertPoint(Jump);

        SmallVector<Value *> NewTailArgs{Jump->getTail()};

        // Add padding so that payload starts at a fixed dword.
        ContHelper::addPaddingValue(DL, Parent->getContext(), NewTailArgs, PayloadStartDword.value());
        // Insert payload into tail args.
        NewTailArgs.push_back(
            Builder.CreateLoad(ArrayType::get(Builder.getInt32Ty(), PayloadSize), PayloadSerializationStorage));

        auto *NewJump = Jump->replaceTail(NewTailArgs);
        ContHelper::OutgoingRegisterCount::setValue(NewJump, PayloadSize);
      }
    }

    /// Create and initialize payload serialization storage for non-Traversal
    /// shader.
    void initializePayloadSerializationStorage(Function *Parent, FunctionData &Data) {
      llvm_dialects::Builder::InsertPointGuard Guard{Builder};
      Builder.SetInsertPointPastAllocas(Parent);
      Data.PayloadStorage = Builder.CreateAlloca(Data.PayloadStorageTy);
      Data.PayloadStorage->setName("payload.serialization.alloca");
      // TODO: We shouldn't need to create the alloca for RGS.
      if (!Data.isRayGeneration() && Data.FirstPayloadArgumentDword.has_value())
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

      // Always ensure that we consider the two dword barycentric coordinates
      // passed as argument for _AmdEnqueueAnyHit calls.
      return getArgumentDwordCount(DL, TraversalDataTy) +
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
                                                             std::optional<uint32_t> PayloadStartDword) {
      Type *PaddingTy = nullptr;
      const uint32_t StartDword = PayloadStartDword.value_or(0);

#ifndef NDEBUG
      LLVM_DEBUG(dbgs() << "Computing padding and payload based on following data:\n"
                        << "Payload size: " << PayloadSizeDwords << " dwords\n"
                        << "Payload start dword: " << StartDword << "\nArgument types:\n");
      for (Type *Ty : ArgTys)
        LLVM_DEBUG(dbgs() << *Ty << ": " << lgc::cps::getArgumentDwordCount(DL, Ty) << " dwords\n");
#endif

      // Compute padding type so that payload starts at a fixed dword.
      // If PayloadStartDword is set to std::nullopt, then we don't pass
      // payload, thus we don't need padding.
      if (PayloadStartDword.has_value()) {
        PaddingTy = ContHelper::getPaddingType(DL, Mod.getContext(), ArgTys, StartDword);
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
  };

  void replaceCall(FunctionData &Data, CallInst *Call, Function *Func, ContinuationCallType CallType);
  void handleExitRayGen(const FunctionData &Data);
  void replaceAwaitCall(ContinuationCallType CallType, CallInst *Call, const FunctionData &Data, Value *PayloadOrAttrs,
                        Type *PayloadOrAttrsTy);
  void replaceReportHitCall(FunctionData &Data, CallInst *Call);

  void replaceShaderIndexCall(FunctionData &Data, CallInst *Call);
  void replaceShaderRecordBufferCall(FunctionData &Data, CallInst *Call);

  void handleGetShaderKind(Function &Func);
  void handleGetCurrentFuncAddr(Function &Func);

  void handleAmdInternalFunc(Function &Func);

  void handleUnrematerializableCandidates();

  void collectGpuRtFunctions();
  void determineDispatchSystemDataType();
  void extendArgumentStruct();

  // Computes an upper bound on the number of required payload registers
  // for a TraceRay call, based on module-wide max attribute and payload size.
  // For intersection shaders, this determines the number of preserved payload registers.
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
  void prepareAnyHitExits(Function *F, FunctionData &Data);
  void processFunctionEnd(FunctionData &Data, FunctionEndData &EData);
  void processFunction(Function *F, FunctionData &FuncData);
  void handleContPayloadRegisterI32Count(Function &F);
  void handleContPayloadRegistersGetI32(Function &F, Function &Parent, FunctionData &Data);
  void handleContPayloadRegistersSetI32(Function &F, Function &Parent, FunctionData &Data);

  void collectProcessableFunctions();

  CallInst *insertCpsAwait(Type *ReturnTy, Value *ShaderAddr, Instruction *Call, ArrayRef<Value *> Args,
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
  /// System data type passed to Traversal
  Type *TraversalDataTy;
  /// System data type passed to ClosestHit and Miss
  Type *HitMissDataTy;
  /// Dispatch system data type passed to RayGen and others
  Type *DispatchSystemDataTy;
  /// Vgpr Argument struct type passed to shaders
  StructType *VgprArgumentStructTy;

  // Function definitions and declarations from HLSL
  // Driver implementation that returns if AcceptHitAndEndSearch was called
  Function *IsEndSearch;
  // Driver implementations to set and get the triangle hit attributes from
  // system data
  Function *GetTriangleHitAttributes;
  Function *SetTriangleHitAttributes;
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

  auto OptMaxUsedPayloadRegisterCount = ContHelper::MaxUsedPayloadRegisterCount::tryGetValue(&Module);
  MaxUsedPayloadRegisterCount = OptMaxUsedPayloadRegisterCount.value_or(0);
  MaxUsedPayloadRegisterCountWasSet = OptMaxUsedPayloadRegisterCount.has_value();

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
}

/// Write the previously derived information about max payload registers and
/// stack address space that was derived by metadata as global state.
void ModuleMetadataState::updateModuleMetadata() const {
  ContHelper::MaxPayloadRegisterCount::setValue(&Mod, MaxPayloadRegisterCount);
  ContHelper::MaxUsedPayloadRegisterCount::setValue(&Mod, MaxUsedPayloadRegisterCount);
  ContHelper::setStackAddrspace(Mod, StackAddrspace);
}

// Create a lgc.cps.await operation for a given shader address.
CallInst *LowerRaytracingPipelinePassImpl::insertCpsAwait(Type *ReturnTy, Value *ShaderAddr, Instruction *Call,
                                                          ArrayRef<Value *> Args, ContinuationCallType CallType,
                                                          RayTracingShaderStage ShaderStage) {
  assert(ShaderAddr->getType() == Builder.getInt32Ty());

  Builder.SetInsertPoint(Call);

  RayTracingShaderStage CallStage = RayTracingShaderStage::Count;
  if (CallType == ContinuationCallType::Traversal)
    CallStage = RayTracingShaderStage::Traversal;
  else if (CallType == ContinuationCallType::CallShader)
    CallStage = RayTracingShaderStage::Callable;
  else if (CallType == ContinuationCallType::AnyHit)
    CallStage = RayTracingShaderStage::AnyHit;

  assert(CallStage != RayTracingShaderStage::Count && "LowerRaytracingPipelinePassImpl::insertCpsAwait: Invalid "
                                                      "call stage before inserting lgc.cps.await operation!");

  return Builder.create<AwaitOp>(ReturnTy, ShaderAddr, 1 << static_cast<uint8_t>(getCpsLevelForShaderStage(CallStage)),
                                 Args);
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

  for (unsigned Idx = 0; Idx < cast<FixedVectorType>(Vector->getType())->getNumElements(); ++Idx)
    Arguments.push_back(B.CreateExtractElement(Vector, B.getInt32(Idx)));

  return Arguments;
}

// Check if @Arg is of fixed vector type. If yes, flatten it into extractelement
// instructions and append them to @Arguments. Return true if @Arguments
// changed, false otherwise.
static bool flattenVectorArgument(IRBuilder<> &B, Value *Arg, SmallVectorImpl<Value *> &Arguments) {
  if (isa<FixedVectorType>(Arg->getType())) {
    const auto &FlattenedArguments = flattenVectorArgument(B, Arg);
    Arguments.append(FlattenedArguments.begin(), FlattenedArguments.end());

    return !FlattenedArguments.empty();
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
        if (FuncName.starts_with("_AmdAwait")) {
          AwaitCalls.push_back(CI);
        } else if (FuncName.starts_with("_AmdAcceptHitAttributes")) {
          AcceptHitAttrsCalls.push_back(CI);
        }
      }
    }
  }

  for (auto *CI : AwaitCalls) {
    Builder.SetInsertPoint(CI);
    replaceAwaitCall(CallType, CI, Data, PayloadOrAttrs, PayloadOrAttrsTy);
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

  Builder.CreateRetVoid();
  Then->eraseFromParent();
}

/// Replace a call to Await with a call to a given address and pass generated
/// token into an await call
void LowerRaytracingPipelinePassImpl::replaceAwaitCall(ContinuationCallType CallType, CallInst *Call,
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
    // For intersection, use number of passed through payload registers.
    ReturnedRegisterCount = Data.NumPassedThroughPayloadDwords;
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
          Data.PayloadStorage, Builder.getPtrTy(Data.PayloadStorage->getType()->getPointerAddressSpace()));

      Builder.CreateStore(LocalPayloadMem, CastPayload);
      // Set stacksize metadata on F
      setStacksizeMetadata(*Call->getFunction(), Data.PayloadSpillSize);
    }
    // Copy local payload to global payload, before await call (e.g. TraceRay,
    // CallShader)
    copyPayload(*PayloadOrAttrsTy, PayloadOrAttrs, Data.PayloadStorage, ShaderStage, PAQAccessKind::Write,
                *OutgoingSerializationLayout);
  }

  auto *FTy = Call->getFunctionType();
  SmallVector<Type *, 2> ArgTys;
  SmallVector<Value *, 2> Args;

  constexpr uint32_t ShaderIndexArgIdx = 1;
  Value *ShaderIndex = Call->getArgOperand(ShaderIndexArgIdx);
  assert(ShaderIndex->getType()->isIntegerTy(32));

  // We need to identify the tail argument list here, since this is what we need to use for computing the padding.
  // That means, the first argument behind the return address is our start index.
  constexpr uint32_t RetAddrArgIdx = ShaderIndexArgIdx + 1;
  Value *RetAddr = Call->getArgOperand(RetAddrArgIdx);
  assert(RetAddr->getType()->isIntegerTy(32));
  constexpr uint32_t TailArgStartIdx = RetAddrArgIdx + 1;

  ArgTys.append(FTy->param_begin() + TailArgStartIdx, FTy->param_end());
  Args.append(Call->arg_begin() + TailArgStartIdx, Call->arg_end());

  if (CallType == ContinuationCallType::AnyHit) {
    // Add hit attributes to arguments
    ArgTys.push_back(PayloadOrAttrsTy);
    auto *HitAttrs = Builder.CreateLoad(PayloadOrAttrsTy, PayloadOrAttrs);
    Args.push_back(HitAttrs);
  }

  uint32_t OutgoingPayloadDwords = 0;
  if (Data.NumPassedThroughPayloadDwords.has_value()) {
    OutgoingPayloadDwords = Data.NumPassedThroughPayloadDwords.value();
  } else {
    OutgoingPayloadDwords = std::min(OutgoingSerializationLayout ? OutgoingSerializationLayout->NumStorageI32s
                                                                 : MetadataState.getMaxPayloadRegisterCount(),
                                     MetadataState.getMaxPayloadRegisterCount());
  }

  const bool HasPayload = Data.FirstPayloadArgumentDword.has_value();

  // Add padding so that returned payload starts at a fixed dword.
  if (HasPayload) {
    const auto &[OutgoingPaddingTy, OutgoingPayloadTy] =
        PayloadHelper.computePaddingAndPayloadArgTys(ArgTys, OutgoingPayloadDwords, Data.FirstPayloadArgumentDword);
    Args.push_back(PoisonValue::get(OutgoingPaddingTy));
    Args.push_back(Builder.CreateLoad(OutgoingPayloadTy, Data.PayloadStorage));
  }

  // Compute padding for the resume function so that payload starts at a
  // fixed dword.
  // Patch the return address into the await call, since it got excluded for
  // the padding computation previously.
  Args.insert(Args.begin(), {ShaderIndex, RetAddr});

  SmallVector<Type *> ReturnedArgTys{Call->getType()};
  if (HasPayload) {
    PayloadHelper.computePaddingAndPayloadArgTys(ReturnedArgTys, ReturnedRegisterCount.value(),
                                                 Data.FirstPayloadArgumentDword);
  }

  // Return shader record index + return address
  ReturnedArgTys.insert(ReturnedArgTys.begin(), {Builder.getInt32Ty(), Builder.getInt32Ty()});
  auto *NewRetTy = StructType::get(Builder.getContext(), ReturnedArgTys);
  auto *ShaderAddr = Call->getArgOperand(0);
  auto *NewCall = insertCpsAwait(NewRetTy, ShaderAddr, Call, Args, CallType, Data.Kind);
  NewCall->copyMetadata(*Call);

  // Copy back returned payload to the payload serialization alloca as part of
  // the payload copying.
  if (HasPayload)
    Builder.CreateStore(Builder.CreateExtractValue(NewCall, ReturnedArgTys.size() - 1), Data.PayloadStorage);

  ContHelper::ReturnedRegisterCount::setValue(NewCall, ReturnedRegisterCount.value());

  // Annotate call with the number of registers used for payload
  ContHelper::OutgoingRegisterCount::setValue(NewCall, OutgoingPayloadDwords);
  if (OutgoingSerializationLayout) {
    MetadataState.updateMaxUsedPayloadRegisterCount(OutgoingPayloadDwords);
    MetadataState.updateMaxUsedPayloadRegisterCount(ReturnedRegisterCount.value());
  }

  if (CallType != ContinuationCallType::AnyHit) {
    // Copy global payload back to local payload
    // Overwrite the local payload with poison first, to make sure it is not
    // seen as live state.
    Builder.CreateStore(Builder.CreateFreeze(PoisonValue::get(PayloadOrAttrsTy)), PayloadOrAttrs);

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
    // Extract the system data from the { %shaderIndex, %rcr, %systemData, %padding, %payload }
    // struct returned by the await call.
    Value *ReturnedSystemData = Builder.CreateExtractValue(NewCall, 2);
    Call->replaceAllUsesWith(ReturnedSystemData);

    // Find (whatever comes first) the last store of the returned system data or a call to lgc.ilcps.setLocalRootIndex.
    // We use this as split point as described below.
    Instruction *SplitPoint = nullptr;
    auto *ParentBB = Call->getParent();
    for (auto It = ParentBB->rbegin(); It != ParentBB->rend(); ++It) {
      if (auto *Store = dyn_cast<StoreInst>(&*It); Store && Store->getValueOperand() == ReturnedSystemData) {
        SplitPoint = Store;
        break;
      }

      if (auto *Idx = dyn_cast<lgc::ilcps::SetLocalRootIndexOp>(&*It)) {
        SplitPoint = Idx;
        break;
      }
    }

    // After the await, we reset local state (system data, potentially the local root index).
    // We need to ensure that any code rematerialized by coro passes to after the suspend point is placed after these
    // restores. As we currently do not have a robust way to achieve that, work around the problem by splitting the BB
    // after the restore code, relying on coro passes to rematerialize within the same BB as the usage.
    if (SplitPoint) {
      auto *Next = &*++SplitPoint->getIterator();
      if (!Next->isTerminator())
        SplitBlock(Next->getParent(), Next);
    }
  }

  Call->eraseFromParent();
}

/// Replace a call to lgc.rt.shader.index with the passed shader index argument.
void LowerRaytracingPipelinePassImpl::replaceShaderIndexCall(FunctionData &Data, CallInst *Call) {
  if (Data.isRayGeneration())
    Call->replaceAllUsesWith(Builder.getInt32(0));
  else
    Call->replaceAllUsesWith(Call->getFunction()->getArg(CpsArgIdx::ShaderIndex));

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
    Builder.SetInsertPoint(&CInst);
    Value *AsContRef = Builder.create<AsContinuationReferenceOp>(F);
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
    auto *DstPtr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(B, I32, Dst, I32Index);
    auto *SrcPtr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(B, I32, Src, I32Index);
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
    auto *DstPtr = CompilerUtils::simplifyingCreateConstGEP1_32(B, I8, Dst, I8Index);
    auto *SrcPtr = CompilerUtils::simplifyingCreateConstGEP1_32(B, I8, Src, I8Index);
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
    auto *SpillPtr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, Builder.getInt8Ty(), PayloadStorage,
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
        auto *LoadPtr = CompilerUtils::simplifyingCreateConstGEP1_32(Builder, I32, PayloadStorage, I);
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
      auto *StorePtr = CompilerUtils::simplifyingCreateConstGEP1_32(Builder, I32, PayloadStorage, I);
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
  auto *RegTyPtr = Builder.getPtrTy(InlineHitAttrsAlloc->getAddressSpace());
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
          Data.PayloadStorage, Builder.getPtrTy(Data.PayloadStorage->getType()->getPointerAddressSpace()));
      // Last zero yields pointer to the first element of the i32 array
      PayloadHitAttrs =
          Builder.CreateInBoundsGEP(Layout->SerializationTy, PayloadSerialization,
                                    {Builder.getInt32(0), Builder.getInt32(0), Builder.getInt32(IndexInterval.Begin)});
      PayloadHitAttrBytes = RegisterBytes * IndexInterval.size();
    } else {
      // Inline attributes suffice, nothing to do.
    }
  } else {
    assert(Data.isIntersection() && "Unexpected shader kind");
    // We are in an intersection shader, which does not know the payload type.
    // Assume maximum possible size
    PayloadHitAttrBytes = MetadataState.getMaxHitAttributeByteCount() - InlineHitAttrsBytes;
    // Use hit attribute storage at fixed index
    PayloadHitAttrs = CompilerUtils::simplifyingCreateConstGEP1_32(Builder, I32, Data.PayloadStorage,
                                                                   FirstPayloadHitAttributeStorageRegister);
  }

  uint64_t HitAttrsBytes = DL->getTypeStoreSize(Data.HitAttributes).getFixedValue();
  if (HitAttrsBytes > MetadataState.getMaxHitAttributeByteCount())
    report_fatal_error("Hit attributes are too large!");
  assert(InlineHitAttrsBytes + PayloadHitAttrBytes >= HitAttrsBytes && "Insufficient hit attribute storage!");
  LocalHitAttributes = Builder.CreateBitCast(LocalHitAttributes, RegTyPtr);
  auto *I8Ty = Builder.getInt8Ty();
  for (unsigned I = 0; I < divideCeil(HitAttrsBytes, RegisterBytes); I++) {
    auto *LocalPtr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, LocalHitAttributes, I);
    Value *GlobalPtr;
    if (I < InlineRegSize)
      GlobalPtr = CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, InlineHitAttrs, I);
    else
      GlobalPtr =
          CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, RegTy, PayloadHitAttrs, I - InlineRegSize);

    auto *LoadPtr = GlobalToLocal ? GlobalPtr : LocalPtr;
    auto *StorePtr = GlobalToLocal ? LocalPtr : GlobalPtr;
    if ((I + 1) * RegisterBytes <= HitAttrsBytes) {
      // Can load a whole register
      auto *Val = Builder.CreateLoad(RegTy, LoadPtr);
      Builder.CreateStore(Val, StorePtr);
    } else {
      // Load byte by byte into a vector and pad the rest with undef
      for (unsigned J = 0; J < HitAttrsBytes % RegisterBytes; J++) {
        auto *Val =
            Builder.CreateLoad(I8Ty, CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, I8Ty, LoadPtr, J));
        Builder.CreateStore(Val, CompilerUtils::simplifyingCreateConstInBoundsGEP1_32(Builder, I8Ty, StorePtr, J));
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
  uint32_t MaxRegisterCount = MetadataState.getNumPassedThroughPayloadDwords();

  struct VisitorState {
    ModuleMetadataState &Metadata;
    uint32_t MaxRegisterCount;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .addSet<lgc::cps::JumpOp>([](VisitorState &State, Instruction &Op) {
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
  if (!Data.isTraversal()) {
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

// Lower lgc.rt.{accept.hit.and.end.search,ignore.hit} intrinsics and insert the default accept hit calls.
void LowerRaytracingPipelinePassImpl::prepareAnyHitExits(Function *F, FunctionData &Data) {
  // First, collect default accept returns.
  SmallVector<ReturnInst *> AcceptReturns;
  for (BasicBlock &BB : *F) {
    if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
      if (Ret != &*BB.begin() && isa<AcceptHitAndEndSearchOp, IgnoreHitOp>(Ret->getPrevNode()))
        continue;

      AcceptReturns.push_back(Ret);
    }
  }

  // Now insert the accept hit calls. This adds new basic blocks, so we do it in a separate loop.
  for (auto *Ret : AcceptReturns) {
    Builder.SetInsertPoint(Ret);
    assert(AcceptHit && "Could not find AcceptHit function");
    auto *SystemDataTy = cast<StructType>(getFuncArgPtrElementType(AcceptHit, 0));
    auto *SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy, SystemDataTy);
    CrossInliner.inlineCall(Builder, AcceptHit, SystemData);

    Data.AnyHitExits.try_emplace(Ret, AnyHitExitKind::AcceptHit);
  }

  // Now collect and do the initial lowering of the intrinsics.
  SmallVector<CallInst *> IntrinsicReturns;
  static const auto Visitor =
      llvm_dialects::VisitorBuilder<SmallVector<CallInst *>>()
          .addSet<AcceptHitAndEndSearchOp, IgnoreHitOp>(
              [](SmallVector<CallInst *> &List, Instruction &I) { List.push_back(cast<CallInst>(&I)); })
          .build();

  Visitor.visit(IntrinsicReturns, *F);

  for (CallInst *I : IntrinsicReturns) {
    // First, ensure that the next instruction is a return.
    Instruction *Next = I->getNextNode();
    ReturnInst *Ret = dyn_cast<ReturnInst>(Next);
    if (!Ret) {
      // unreachable should be a common next instruction since these ops are noreturn.
      // If we don't have that, split the block -- everything after the intrinsic
      // will become unreachable.
      if (!isa<UnreachableInst>(Next)) {
        BasicBlock *NewBB = I->getParent()->splitBasicBlockBefore(Next);
        NewBB->takeName(Next->getParent());
        Next = NewBB->getTerminator();
      }

      Builder.SetInsertPoint(Next);
      Ret = Builder.CreateRetVoid();
      Next->eraseFromParent();
    }

    [[maybe_unused]] bool Inserted =
        Data.AnyHitExits
            .try_emplace(Ret, isa<AcceptHitAndEndSearchOp>(I) ? AnyHitExitKind::AcceptHitAndEndSearch
                                                              : AnyHitExitKind::IgnoreHit)
            .second;
    assert(Inserted);

    // Now replace the intrinsic
    replaceIntrinsicCall(Builder, Data.SystemDataTy, Data.SystemData, Data.Kind, I, GpurtLibrary, CrossInliner);
  }
}

void LowerRaytracingPipelinePassImpl::processFunctionEnd(FunctionData &Data, FunctionEndData &EData) {
  AnyHitExitKind AHExitKind = AnyHitExitKind::None;
  bool IsAnyHit = Data.isAnyHit();

  Builder.SetInsertPoint(EData.Terminator);

  auto *PayloadTy = Data.IncomingPayload;
  if (!Data.isRayGeneration() && !Data.isIntersection() && !Data.isTraversal()) {
    assert(PayloadTy && "Missing payload type!");

    if (IsAnyHit) {
      auto It = Data.AnyHitExits.find(EData.Terminator);
      assert(It != Data.AnyHitExits.end());
      AHExitKind = It->second;

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

  if (Data.isRayGeneration()) {
    assert(!RetValue && "RayGen cannot return anything");
    if (ExitRayGen)
      handleExitRayGen(Data);

    Builder.create<lgc::cps::CompleteOp>();
    Builder.CreateUnreachable();
    EData.Terminator->eraseFromParent();

    return;
  }

  SmallVector<Value *> PaddingArgs;
  SmallVector<Value *> TailArgList;
  Value *DummyI32 = PoisonValue::get(I32);

  Function *Parent = EData.Terminator->getFunction();

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

  Value *ReturnAddr = Parent->getArg(CpsArgIdx::ReturnAddr);

  if (RetValue)
    PaddingArgs.push_back(RetValue);

  // Construct the tail argument list and append the padding and payload
  // values.
  TailArgList.append(PaddingArgs);
  PayloadHelper.appendPaddingAndPayloadValues(PaddingArgs, TailArgList, OutgoingRegisterCount,
                                              Data.FirstPayloadArgumentDword, Data.PayloadStorage);

  Instruction *Jump = Builder.create<lgc::cps::JumpOp>(ReturnAddr, getPotentialCpsReturnLevels(Data.Kind), DummyI32,
                                                       DummyI32, DummyI32, TailArgList);
  Builder.CreateUnreachable();
  EData.Terminator->eraseFromParent();

  // Annotate the terminator with number of outgoing payload registers.
  // This annotation will be passed along the following transformations,
  // ending up at the final continuation call.
  ContHelper::OutgoingRegisterCount::setValue(Jump, OutgoingRegisterCount);
  if (EData.OutgoingSerializationLayout)
    MetadataState.updateMaxUsedPayloadRegisterCount(OutgoingRegisterCount);
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

  // Create the CPS function header.

  // A CPS function signature consists of:
  //  * Shader index
  //  * Return continuation reference (RCR): i32
  //  * Remaining arguments (system data, optionally hit attributes)
  // We need to determine the starting dword of payload storage in arguments,
  // so that payload starts at a fixed VGPR across all shaders in a pipeline.
  // The overall layout is:
  // | shaderIndex | returnAddr | systemData | hitAttrs | padding | payload |
  // For systemData and hitAttrs, use the max possible sizes for calculation.
  // We always have return address and shader index arguments, which must not be included in the padding computation.

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
    Data.NumPassedThroughPayloadDwords = getUpperBoundOnTraceRayPayloadRegisters();
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
    SystemDataTy = F->getArg(0)->getType();

    AllArgTypes.push_back(SystemDataTy);
    NewRetTy = SystemDataTy;

    Data.NumPassedThroughPayloadDwords = MetadataState.getNumPassedThroughPayloadDwords();

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

  if (!Data.isRayGeneration()) {
    if (!Data.isAnyHit()) {
      // Add a dummy argument for CpsArgIdx::HitAttributes so that the arg index
      // of payload matches CpsArgIdx::Payload
      AllArgTypes.push_back(StructType::get(*Context, {}));
    }

    PayloadHelper.computePaddingAndPayloadArgTys(AllArgTypes, NumIncomingPayloadDwords.value(),
                                                 Data.FirstPayloadArgumentDword);
  }

  // Pass in the shader index and return address arguments so they don't get included in the padding.
  AllArgTypes.insert(AllArgTypes.begin(), {Builder.getInt32Ty(), Builder.getInt32Ty()});

  Data.PayloadSpillSize =
      computePayloadSpillSize(Data.MaxOutgoingPayloadI32s, MetadataState.getMaxPayloadRegisterCount());
  assert(Data.PayloadSpillSize == 0 || !Data.isIntersection());

  // Create new function to change signature
  auto *NewFuncTy = FunctionType::get(Builder.getVoidTy(), AllArgTypes, false);
  Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ArrayRef<AttributeSet>{});
  NewFunc->takeName(F);
  // FIXME: Remove !pointeetypes metadata to workaround an llvm bug. If struct types
  // are referenced only from metadata, LLVM omits the type declaration when
  // printing IR and fails to read it back in because of an unknown type.
  NewFunc->setMetadata("pointeetys", nullptr);

  llvm::moveFunctionBody(*F, *NewFunc);

  Data.SystemDataTy = cast<StructType>(SystemDataTy);
  processFunctionEntry(Data, NewFunc->getArg(CpsArgIdx::SystemData));

  // Mark as CPS function with the corresponding level.
  CpsLevel Level = getCpsLevelForShaderStage(Data.Kind);
  setCpsFunctionLevel(*NewFunc, Level);

  if (!Data.isRayGeneration()) {
    NewFunc->getArg(CpsArgIdx::SystemData)->setName("system.data");
    NewFunc->getArg(CpsArgIdx::HitAttributes)->setName("hit.attrs");
    NewFunc->getArg(CpsArgIdx::Padding)->setName("padding");
    NewFunc->getArg(CpsArgIdx::Payload)->setName("payload");
  }

  Value *NewSystemData = nullptr;
  if (Data.isTraversal()) {
    assert(F->arg_size() == 1);
    NewSystemData = NewFunc->getArg(CpsArgIdx::SystemData);
  }

  PayloadHelper.initializePayloadSerializationStorage(NewFunc, Data);

  if (auto *ContPayloadRegistersGetI32 = Mod->getFunction("_AmdContPayloadRegistersGetI32"))
    handleContPayloadRegistersGetI32(*ContPayloadRegistersGetI32, *NewFunc, Data);

  if (auto *ContPayloadRegistersSetI32 = Mod->getFunction("_AmdContPayloadRegistersSetI32"))
    handleContPayloadRegistersSetI32(*ContPayloadRegistersSetI32, *NewFunc, Data);

  if (NewSystemData)
    F->getArg(0)->replaceAllUsesWith(NewSystemData);

  NewFunc->getArg(CpsArgIdx::ShaderIndex)->setName("shaderIndex");
  NewFunc->getArg(CpsArgIdx::ReturnAddr)->setName("returnAddr");

  FunctionEndData EData;
  if (Data.isRayGeneration()) {
    // Entry functions have no incoming payload or continuation state
    ContHelper::IncomingRegisterCount::setValue(NewFunc, 0);
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
    assert((Data.isCallable() || SerializationInfo == nullptr ||
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

    if (!Data.isIntersection() && !Data.isTraversal()) {
      assert(PayloadTy && "Missing payload type!");

      // For AnyHit, the layout depends on whether we accept or ignore, which
      // we do not know yet. In that case, the layout is determined later.
      if (!Data.isAnyHit()) {
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
      // Annotate function with the number of registers for incoming payload
      ContHelper::IncomingRegisterCount::setValue(NewFunc, IncomingRegisterCount);

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
      if (Data.isAnyHit()) {
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

        // Copy new hit attributes from argument
        Builder.CreateStore(NewFunc->getArg(CpsArgIdx::HitAttributes), HitAttrsAlloca);
        HitAttrs->replaceAllUsesWith(HitAttrsAlloca);
      } else if (Data.isClosestHit()) {
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
    } else if (Data.isIntersection()) {
      // Annotate intersection shader with the maximum number of registers
      // used for payload
      // TODO: When compiling a pipeline and not a library, we could figure
      //       out the pipeline-wide max (on a higher level than here) and
      //       use that instead. For a library compile, we can't know the
      //       max payload size of shaders in pipelines this shader is used
      //       in.
      ContHelper::IncomingRegisterCount::setValue(NewFunc, Data.NumPassedThroughPayloadDwords.value());
      // Intentionally do NOT update MaxUsedPayloadRegisterCount
    } else {
      assert(Data.isTraversal());
      // Intentionally do nothing for Traversal. We explicitly add Traversal
      // register count metadata elsewhere.
    }

    EData.OutgoingSerializationLayout = OutgoingSerializationLayout;
    EData.SavedRegisterValues = std::move(SavedRegisterValues);
    EData.NewPayload = NewPayload;
    EData.ShaderStage = ShaderStage;
    EData.HitAttrsAlloca = HitAttrsAlloca;
    EData.OrigHitAttrsAlloca = OrigHitAttrsAlloca;
  }
  Data.ReturnTy = NewRetTy;

  if (Data.isAnyHit())
    prepareAnyHitExits(NewFunc, Data);

  if (Data.isTraversal()) {
    PayloadHelper.patchJumpCalls(NewFunc, Data.JumpCalls, Data.FirstPayloadArgumentDword,
                                 Data.NumPassedThroughPayloadDwords, Data.PayloadStorage);
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

  // Replace ShaderRecordBufferOp calls
  for (auto *Call : Data.ShaderRecordBufferCalls) {
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceShaderRecordBufferCall(Data, Call);
  }

  // Replace non-rematerializable intrinsic calls
  for (auto *Call : Data.IntrinsicCalls)
    replaceIntrinsicCall(Builder, Data.SystemDataTy, Data.SystemData, Data.Kind, Call, GpurtLibrary, CrossInliner);

  // Modify function ends
  // We do this close to the end because ReportHit handling can insert new returns.
  if (!Data.isTraversal()) {
    // While iterating over function ends, basic blocks are inserted by inlining
    // functions, so we copy them beforehand.
    SmallVector<BasicBlock *> BBs(make_pointer_range(*NewFunc));
    for (auto *BB : BBs) {
      auto *I = BB->getTerminator();
      assert(I && "BB must have terminator");
      // Replace the end of the BB if it terminates the function
      if (auto *Ret = dyn_cast<ReturnInst>(I)) {
        EData.Terminator = Ret;
        processFunctionEnd(Data, EData);
      }
    }
  }

  // Lower lgc.rt.shader.index to the shader index argument or 0.
  static const auto ShaderIndexVisitor =
      llvm_dialects::VisitorBuilder<SmallVector<ShaderIndexOp *>>()
          .add<lgc::rt::ShaderIndexOp>([](SmallVector<ShaderIndexOp *> &ShaderIndexCalls, lgc::rt::ShaderIndexOp &Op) {
            ShaderIndexCalls.push_back(&Op);
          })
          .build();

  SmallVector<lgc::rt::ShaderIndexOp *> ShaderIndexCalls;
  ShaderIndexVisitor.visit(ShaderIndexCalls, *F);

  for (auto *Call : ShaderIndexCalls)
    replaceShaderIndexCall(Data, Call);

#ifndef NDEBUG
  if (!Data.isRayGeneration()) {
    // Check that all returns have registercount metadata
    for (const auto &BB : *F) {
      auto *Terminator = BB.getTerminator();
      if (Terminator->getOpcode() == Instruction::Ret) {
        // Traversal needs to end with jumps + unreachable
        if (Data.isTraversal())
          report_fatal_error("Disallowed return found in Traversal, all code paths need to end with an Enqueue");
        else if (!ContHelper::OutgoingRegisterCount::tryGetValue(Terminator))
          report_fatal_error("Missing registercount metadata!");
      }
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

      if (!Data.isIntersection() && !Data.isRayGeneration() && !Data.isTraversal()) {
        assert(!Func.arg_empty() && "Shader must have at least one argument");
        Data.IncomingPayload = getFuncArgPtrElementType(&Func, 0);
        PAQPayloadConfig PAQConfig = {Data.IncomingPayload, MetadataState.getMaxHitAttributeByteCount()};
        Data.IncomingPayloadSerializationInfo = &PAQManager.getOrCreateSerializationInfo(PAQConfig, Kind);
        assert(Data.IncomingPayloadSerializationInfo != nullptr && "Missing serialization info!");
      }
      if (Data.isAnyHit() || Data.isClosestHit()) {
        assert(Func.arg_size() >= 2 && "Shader must have at least two arguments");
        Data.HitAttributes = getFuncArgPtrElementType(&Func, Func.arg_size() - 1);
      }

      if (Data.isIntersection()) {
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

  if (FuncName.starts_with("_AmdGetFuncAddr")) {
    ContHelper::handleGetFuncAddr(Func, Builder);
  } else if (FuncName.starts_with("_AmdGetShaderKind")) {
    handleGetShaderKind(Func);
  } else if (FuncName.starts_with("_AmdGetCurrentFuncAddr")) {
    handleGetCurrentFuncAddr(Func);
  }
}

// Search for known intrinsics that cannot be rematerialized
void LowerRaytracingPipelinePassImpl::handleUnrematerializableCandidates() {
  for (auto &Func : *Mod) {
    if (!lgc::rt::LgcRtDialect::isDialectOp(Func))
      continue;

    static const llvm_dialects::OpSet NonRematerializableDialectOps =
        llvm_dialects::OpSet::get<TraceRayOp, ReportHitOp, CallCallableShaderOp, ShaderIndexOp, ShaderRecordBufferOp,
                                  JumpOp, AcceptHitAndEndSearchOp, IgnoreHitOp>();
    if (!NonRematerializableDialectOps.contains(Func)) {
      llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
        auto Data = ToProcess.find(CInst.getFunction());
        if (Data != ToProcess.end()) {
          if (!rematsupport::isRematerializableLgcRtOp(CInst, Data->second.Kind))
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

void LowerRaytracingPipelinePassImpl::determineDispatchSystemDataType() {
  Function *DispatchRaysIndex = GpurtLibrary->getFunction(ContDriverFunc::DispatchRaysIndex3Name);
  assert(DispatchRaysIndex &&
         "LowerRaytracingPipelinePassImpl::determineDispatchSystemDataType: Could not find _cont_DispatchRaysIndex3!");

  DispatchSystemDataTy = getFuncArgPtrElementType(DispatchRaysIndex, 0);
  assert(DispatchSystemDataTy && "LowerRaytracingPipelinePassImpl::determineDispatchSystemDataType: Could "
                                 "not derive DispatchSystemData "
                                 "type from _cont_DispatchRaysIndex3!");
}

/// Try to find the scheduler function on the GPURT module. Extract the arguments struct type, create a new one extended
/// by the maximum number of hit attribute and payload dwords and update the pointee type on the scheduler.
void LowerRaytracingPipelinePassImpl::extendArgumentStruct() {
  Function *SchedulerFunc = GpurtLibrary->getFunction(ContDriverFunc::SchedulerName);
  if (!SchedulerFunc)
    return;

  assert(SchedulerFunc->arg_size() == 1);

  StructType *ExistingStructTy = cast<StructType>(getFuncArgPtrElementType(SchedulerFunc->getArg(0)));

  // Ensure the arguments struct is properly setup if it exists.
  SmallVector<Type *> ArgumentTypes{ExistingStructTy->elements()};
  assert(ArgumentTypes.size() == 5);
  assert(ArgumentTypes[0]->isIntegerTy(32));
  assert(ArgumentTypes[1]->isIntegerTy(32));
  assert(ArgumentTypes[2]->isIntegerTy(32));
  assert(ArgumentTypes[3]->isIntegerTy(32));
  assert(ArgumentTypes[4]->isStructTy());

  ArgumentTypes.push_back(
      ArrayType::get(Builder.getInt32Ty(), MetadataState.getMaxHitAttributeByteCount() / RegisterBytes));
  ArgumentTypes.push_back(ArrayType::get(Builder.getInt32Ty(), MetadataState.getMaxPayloadRegisterCount()));

  // Create a new struct.
  LLVMContext &C = SchedulerFunc->getContext();
  VgprArgumentStructTy =
      StructType::create(C, ArgumentTypes, ExistingStructTy->getStructName(), ExistingStructTy->isPacked());

  // Update the pointee type on the scheduler function.
  SchedulerFunc->eraseMetadata(C.getMDKindID(TypedFuncTy::MDTypesName));
  SmallVector<Metadata *> PointeeTys{ConstantAsMetadata::get(PoisonValue::get(VgprArgumentStructTy))};
  SchedulerFunc->setMetadata(TypedFuncTy::MDTypesName, MDTuple::get(C, PointeeTys));
}

LowerRaytracingPipelinePassImpl::LowerRaytracingPipelinePassImpl(llvm::Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Context{&M.getContext()}, DL{&M.getDataLayout()},
      Builder{Mod->getContext()}, MetadataState{*Mod},
      PAQManager{Mod, &GpurtLibrary, MetadataState.getMaxPayloadRegisterCount()}, PayloadHelper{*Mod, *DL, Builder} {
}

PreservedAnalyses LowerRaytracingPipelinePassImpl::run() {
  collectGpuRtFunctions();
  determineDispatchSystemDataType();
  extendArgumentStruct();

  collectProcessableFunctions();

  struct VisitorState {
    PAQSerializationInfoManager &PAQManager;
    MapVector<Function *, FunctionData> &Processables;
    ModuleMetadataState &Metadata;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .addSet<TraceRayOp, CallCallableShaderOp, ReportHitOp, ShaderRecordBufferOp, JumpOp>([](VisitorState &State,
                                                                                                  Instruction &Op) {
            auto *CInst = cast<CallInst>(&Op);
            auto Data = State.Processables.find(CInst->getFunction());
            if (Data == State.Processables.end())
              return;

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

              Data->second.MaxOutgoingPayloadI32s = std::max(Data->second.MaxOutgoingPayloadI32s, PayloadStorageI32s);
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
