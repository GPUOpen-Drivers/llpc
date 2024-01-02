/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- LowerRaytracingPipeline.cpp -----------------===//
//
// This file implements the frontend part for coroutine support for lgc.rt ops.
// - Add a global for the continuation stack pointer.
// - Introduce a global for the payload. Mark the payload global to be
//   transformed by the RegisterBufferPass.
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
#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "continuations/ContinuationsUtil.h"
#include "continuations/PayloadAccessQualifiers.h"
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
#include "llvm/IR/Verifier.h"
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
    auto *I32 = Type::getInt32Ty(M.getContext());
    // Pointer to the node field in the local payload
    auto *LocalFieldPtr =
        B.CreateInBoundsGEP(&PayloadTy, LocalPayload, PayloadIdxList);

    // If the field is serialized in multiple intervals in the global,
    // we perform a manual bytewise copy using i32 and i8.
    // However, if the field is serialized using a single, contiguous interval
    // and does not have stricter alignment requirements than i32,
    // then we can just load/store the field type from/to the global storage.
    //
    // We currently restrict this mechanism to single-DWord fields to avoid
    // issues with the RegisterBuffer pass which struggles with loads and stores
    // of large vector types, leading to bad IR with additional allocas.
    // TODO: Remove this restriction once we have moved to LLPC-style
    //       continuations without the RegisterBuffer pass.
    const DataLayout &DL = M.getDataLayout();
    if (Intervals.size() == 1 &&
        DL.getABITypeAlign(FieldTy) <= DL.getABITypeAlign(I32) &&
        Intervals[0].size() == 1) {

      // Do a single load+store
      Value *Src = LocalFieldPtr;

      auto *GlobalIntervalI32Ptr = B.CreateInBoundsGEP(
          Layout->SerializationTy, Serialization,
          {B.getInt32(0), B.getInt32(0), B.getInt32(Intervals[0].Begin)});
      Value *Dst = B.CreateBitCast(
          GlobalIntervalI32Ptr,
          FieldTy->getPointerTo(
              GlobalIntervalI32Ptr->getType()->getPointerAddressSpace()));

      if (GlobalAccessKind != PAQAccessKind::Write)
        std::swap(Src, Dst);

      auto *Val = B.CreateLoad(FieldTy, Src);
      B.CreateStore(Val, Dst);
      return;
    }

    // I32 pointer to start of field in local payload
    Value *FieldI32Ptr = B.CreateBitCast(
        LocalFieldPtr,
        I32->getPointerTo(LocalFieldPtr->getType()->getPointerAddressSpace()));

    // Counts how many bytes have already been copied
    unsigned FieldByteOffset = 0;
    unsigned FieldNumBytes =
        M.getDataLayout().getTypeStoreSize(FieldTy).getFixedValue();
    for (unsigned IntervalIdx = 0; IntervalIdx < Intervals.size();
         ++IntervalIdx) {
      const PAQIndexInterval &Interval = Intervals[IntervalIdx];
      // I32 pointer to start of current interval in global payload
      auto *GlobalIntervalI32Ptr = B.CreateInBoundsGEP(
          Layout->SerializationTy, Serialization,
          {B.getInt32(0), B.getInt32(0), B.getInt32(Interval.Begin)});
      // Obtain i32-based index from byte-offset. We only expect
      // to increase FieldByteOffset by a non-multiple of RegisterBytes
      // in the last iteration, so here it should always be divisible
      unsigned FieldI32Offset = divideCeil(FieldByteOffset, RegisterBytes);
      assert(FieldByteOffset == FieldI32Offset * RegisterBytes);
      // I32 pointer into field, offset by FieldI32Offset
      auto *FieldIntervalI32Ptr =
          B.CreateConstGEP1_32(I32, FieldI32Ptr, FieldI32Offset);

      // Determine Src and Dst
      auto *Src = FieldIntervalI32Ptr;
      auto *Dst = GlobalIntervalI32Ptr;
      if (GlobalAccessKind != PAQAccessKind::Write)
        std::swap(Src, Dst);

      unsigned NumCopyBytes = RegisterBytes * Interval.size();

      unsigned FieldNumRemainingBytes = FieldNumBytes - FieldByteOffset;
      if (NumCopyBytes > FieldNumRemainingBytes) {
        assert(IntervalIdx + 1 == Intervals.size() &&
               "Partial storage interval is only allowed for last interval!");
        NumCopyBytes = FieldNumRemainingBytes;
      }

      copyBytes(B, Dst, Src, NumCopyBytes);
      FieldByteOffset += NumCopyBytes;
    }

    assert(FieldByteOffset == FieldNumBytes && "Inconsistent storage size!");
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

  uint32_t getMaxPayloadRegisterCount() const {
    return MaxPayloadRegisterCount;
  }

  uint32_t getMinPayloadRegisterCount() const {
    return MinPayloadRegisterCount;
  }

  ContStackAddrspace getContStackAddrspace() const { return StackAddrspace; }

  void updateModuleMetadata() const;

private:
  Module &Mod;
  /// MaxPayloadRegisterCount is initialized from metadata. If there is none,
  /// use this default instead:
  static constexpr uint32_t DefaultPayloadRegisterCount = 30;
  /// Maximum allowed number of registers to be used for the payload.
  uint32_t MaxPayloadRegisterCount = 0;
  /// Minimum required number of payload registers.
  uint32_t MinPayloadRegisterCount = 0;
  /// The address space used for the continuations stack.
  /// Either stack or global memory.
  ContStackAddrspace StackAddrspace = DXILContHelper::DefaultStackAddrspace;
};

class CpsMutator final {
public:
  explicit CpsMutator(Module &Mod)
      : Mod{Mod}, IsModuleInCpsMode{DXILContHelper::isLgcCpsModule(Mod)},
        Builder{std::make_unique<llvm_dialects::Builder>(Mod.getContext())} {}

  Value *insertCpsAwait(Type *ReturnTy, Value *ShaderAddr, Instruction *Call,
                        ArrayRef<Value *> Args, ContinuationCallType CallType,
                        lgc::cps::CpsShaderStage ShaderStage);

  bool shouldRun() const { return IsModuleInCpsMode; }

private:
  Module &Mod;
  bool IsModuleInCpsMode = false;
  std::unique_ptr<llvm_dialects::Builder> Builder;
};

class LowerRaytracingPipelinePassImpl final {
public:
  LowerRaytracingPipelinePassImpl(Module &M, Module &GpurtLibrary);
  bool run();

private:
  struct FunctionConfig {
    // Maximum allowed size of hit attributes to be used in a TraceRay together
    // with this function, even if this function does not touch hit attributes
    // (e.g. a Miss shader).
    uint32_t MaxHitAttributeBytes = 0;

    bool operator==(const FunctionConfig &Other) const {
      return MaxHitAttributeBytes == Other.MaxHitAttributeBytes;
    }
  };

  struct FunctionData {
    DXILShaderKind Kind = DXILShaderKind::Invalid;
    SmallVector<CallInst *> TraceRayCalls;
    SmallVector<CallInst *> ReportHitCalls;
    SmallVector<CallInst *> CallShaderCalls;
    /// Calls to hlsl intrinsics that cannot be rematerialized
    SmallVector<CallInst *> IntrinsicCalls;
    SmallVector<CallInst *> ShaderIndexCalls;

    /// Pointer to the alloca'd system data object in this function
    AllocaInst *SystemData = nullptr;
    StructType *SystemDataTy = nullptr;
    Type *ReturnTy = nullptr;
    /// Maximum number of I32s required to store the outgoing payload in all
    /// CallShader or TraceRay (maximum over all TraceRay formats) calls
    uint32_t MaxOutgoingPayloadI32s = 0;
    /// Size of the CPS stack allocation used for spilled parts of the payload.
    /// This size is large enough for all used outgoing payload types.
    int32_t PayloadSpillSize = 0;
    /// Type of the incoming payload
    Type *IncomingPayload = nullptr;
    FunctionConfig FuncConfig = {};
    /// Serialization info for the incoming payload, if there is one.
    /// Also applies to the outgoing payload in that case.
    PAQSerializationInfoBase *IncomingPayloadSerializationInfo = nullptr;
    /// hit attributes type, incoming for AnyHit and ClosestHit, outgoing for
    /// Intersection
    Type *HitAttributes = nullptr;
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

  static DXILShaderKind callTypeToShaderKind(ContinuationCallType CallType);

  void replaceCall(FunctionData &Data, CallInst *Call, Function *Func,
                   ContinuationCallType CallType);
  void handleRestoreSystemData(CallInst *Call);
  void replaceContinuationCall(ContinuationCallType CallType, CallInst *Call,
                               const FunctionData &Data, Value *PayloadOrAttrs,
                               Type *PayloadOrAttrsTy);
  void replaceReportHitCall(FunctionData &Data, CallInst *Call);

  void handleReportHit(FunctionData &Data, Function &F);

  void replaceShaderIndexCall(FunctionData &Data, CallInst *Call);

  void handleGetFuncAddr(Function &Func);
  void handleGetShaderKind(Function &Func);
  void handleGetCurrentFuncAddr(Function &Func);

  void handleAmdInternalFunc(Function &Func);

  void splitRestoreBB();

  void handleUnrematerializableCandidates();

  void collectDriverFunctions();

  // Copy the payload content between global payload and local payload.
  // Excludes the stack pointer or hit attributes which may also reside in
  // payload storage. If Stage is not set, all fields in SerializationInfo are
  // copied. Used for CallShader accesses which are not PAQ qualified and do not
  // have PAQShaderStage values.
  // If CopiedNodes is set, nodes contained will not be copied, and all copied
  // nodes are added to it.
  void copyPayload(Type &PayloadTy, Value *LocalPayload,
                   std::optional<PAQShaderStage> Stage,
                   PAQAccessKind GlobalAccessKind,
                   const PAQSerializationLayout &Layout,
                   SmallDenseSet<const PAQNode *, 16> *CopiedNodes = nullptr);

  // Special handling for case of copying the result payload of a traceray call
  // back to the local payload of the caller.
  // This is needed to implement the ClosestHitOut/MissOut optimization.
  // We first perform a copy using the ClosestHitOut layout, and then perform an
  // additional copy using the MissOut layout, skipping any fields already
  // copied (i.e. only copying write(miss) : read(caller) fields).
  void copyTraceRayPayloadIncomingToCaller(
      const PAQTraceRaySerializationInfo &PAQSerializationInfo,
      Value *LocalPayload);

  // Caller-save payload registers before CallShader() or TraceRay(),
  // which can override payload registers. A register needs to be saved
  // if it is live in OutgoingLayout, and not written in OutgoingLayout.
  // This includes the payload memory pointer if present.
  // SavedRegisters maps indices of payload registers to their saved values.
  void savePayloadRegistersBeforeRecursion(
      DXILShaderKind Kind, const PAQSerializationLayout &IncomingLayout,
      const PAQSerializationLayout &OutgoingLayout,
      SmallVectorImpl<Value *> &SavedRegisterValues);

  // Restore previously saved registers.
  void restorePayloadRegistersAfterRecursion(
      const SmallVectorImpl<Value *> &SavedRegisterValues);

  void createPayloadGlobal();

  void setTraversalRegisterCountMetadata();

  void copyHitAttributes(FunctionData &Data, Value *SystemData,
                         Type *SystemDataTy, Value *LocalHitAttributes,
                         bool GlobalToLocal,
                         const PAQSerializationLayout *Layout);
  void processContinuations();
  void processFunctionEntry(Function *F, FunctionData &Data);
  void processFunctionEnd(FunctionData &Data, FunctionEndData &EData);
  void processFunction(Function *F, FunctionData &FuncData);

  void collectProcessableFunctions();

  void handleDriverFuncAssertions();

  constexpr static uint32_t ArgContState = 0;
  constexpr static uint32_t ArgReturnAddr = 1;
  constexpr static uint32_t ArgShaderIndex = 2;
  [[maybe_unused]] constexpr static uint32_t ArgSystemData = 3;
  [[maybe_unused]] constexpr static uint32_t ArgHitAttributes = 4;

  MapVector<Function *, FunctionData> ToProcess;
  Module *Mod;
  Module *GpurtLibrary;
  LLVMContext *Context;
  const DataLayout *DL;
  llvm_dialects::Builder Builder;
  ModuleMetadataState MetadataState;
  CpsMutator Mutator;
  PAQSerializationInfoManager PAQManager;
  CompilerUtils::CrossModuleInliner CrossInliner;
  Type *I32;
  Type *TokenTy;
  /// System data type passed to Traversal
  Type *TraversalDataTy;
  /// System data type passed to ClosestHit and Miss
  Type *HitMissDataTy;
  GlobalVariable *Payload;

  // Function definitions and declarations from HLSL
  // Driver implementation that returns if AcceptHitAndEndSearch was called
  Function *IsEndSearch;
  // Driver implementations to set and get the triangle hit attributes from
  // system data
  Function *GetTriangleHitAttributes;
  Function *SetTriangleHitAttributes;
  Function *GetLocalRootIndex;
  Function *SetLocalRootIndex;
  Function *SetupRayGen;
  Function *TraceRay;
  Function *CallShader;
  Function *ReportHit;
  Function *AcceptHit;

  Function *RegisterBufferSetPointerBarrier;
};

} // namespace

constexpr unsigned ModuleMetadataState::DefaultPayloadRegisterCount;

ModuleMetadataState::ModuleMetadataState(Module &Module) : Mod{Module} {
  // Import PayloadRegisterCount from metadata if set,
  // otherwise from default
  auto RegisterCountFromMD =
      DXILContHelper::tryGetMaxPayloadRegisterCount(Module);
  MaxPayloadRegisterCount =
      RegisterCountFromMD.value_or(DefaultPayloadRegisterCount);

  // Check that if there is a required minimum number of payload registers,
  // it is compatible
  auto MinRegisterCountFromMD =
      DXILContHelper::tryGetMinPayloadRegisterCount(Module);
  MinPayloadRegisterCount =
      MinRegisterCountFromMD.value_or(MaxPayloadRegisterCount);
  assert(MinPayloadRegisterCount <= MaxPayloadRegisterCount);

  // Import StackAddrspace from metadata if set, otherwise from default
  auto StackAddrspaceMD = DXILContHelper::tryGetStackAddrspace(Module);
  StackAddrspace =
      StackAddrspaceMD.value_or(DXILContHelper::DefaultStackAddrspace);
}

/// Write the previously derived information about max payload registers and
/// stack address space that was derived by metadata as global state.
void ModuleMetadataState::updateModuleMetadata() const {
  DXILContHelper::setMaxPayloadRegisterCount(Mod, MaxPayloadRegisterCount);
  DXILContHelper::setStackAddrspace(Mod, StackAddrspace);
}

lgc::cps::CpsShaderStage
convertShaderKindToCpsShaderStage(DXILShaderKind Kind) {
  switch (Kind) {
  case DXILShaderKind::RayGeneration:
    return CpsShaderStage::RayGen;
  case DXILShaderKind::Intersection:
    return CpsShaderStage::Intersection;
  case DXILShaderKind::AnyHit:
    return CpsShaderStage::AnyHit;
  case DXILShaderKind::ClosestHit:
    return CpsShaderStage::ClosestHit;
  case DXILShaderKind::Miss:
    return CpsShaderStage::Miss;
  case DXILShaderKind::Callable:
    return CpsShaderStage::Callable;
  default:
    llvm_unreachable(
        "convertShaderKindToCpsShaderStage: Invalid shader kind provided!");
    break;
  }
}

// Create a lgc.cps.await operation for a given shader address.
Value *CpsMutator::insertCpsAwait(Type *ReturnTy, Value *ShaderAddr,
                                  Instruction *Call, ArrayRef<Value *> Args,
                                  ContinuationCallType CallType,
                                  CpsShaderStage ShaderStage) {
  Builder->SetInsertPoint(Call);

  Value *CR = nullptr;
  if (ShaderAddr->getType()->getIntegerBitWidth() == 64)
    CR = Builder->CreateTrunc(ShaderAddr, Type::getInt32Ty(Mod.getContext()));
  else
    CR = ShaderAddr;

  CpsShaderStage CallStage = CpsShaderStage::Count;
  if (CallType == ContinuationCallType::Traversal)
    CallStage = CpsShaderStage::Traversal;
  else if (CallType == ContinuationCallType::CallShader)
    CallStage = CpsShaderStage::Callable;
  else if (CallType == ContinuationCallType::AnyHit)
    CallStage = CpsShaderStage::AnyHit;

  assert(CallStage != CpsShaderStage::Count &&
         "LowerRaytracingPipelinePassImpl::CpsMutator::insertCpsAwait: Invalid "
         "call stage before inserting lgc.cps.await operation!");

  return Builder->create<AwaitOp>(
      ReturnTy, CR,
      1 << static_cast<uint8_t>(getCpsLevelForShaderStage(CallStage)), Args);
}

Function *llvm::getSetLocalRootIndex(Module &M) {
  auto *Name = "amd.dx.setLocalRootIndex";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *I32 = Type::getInt32Ty(C);
  auto *FuncTy = FunctionType::get(Void, {I32}, false);
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
}

// Set maximum continuation stack size metadata
static void setStacksizeMetadata(Function &F, uint64_t NeededStackSize) {
  uint64_t CurStackSize = DXILContHelper::tryGetStackSize(&F).value_or(0);
  if (NeededStackSize > CurStackSize)
    DXILContHelper::setStackSize(&F, NeededStackSize);
}

// Create an ExtractElement instruction for each index of a FixedVector @Vector
// and return it.
static SmallVector<Value *, 3> flattenVectorArgument(IRBuilder<> &B,
                                                     Value *Vector) {
  assert(isa<FixedVectorType>(Vector->getType()) && "Not a FixedVectorType!");

  SmallVector<Value *, 3> Arguments;

  for (unsigned Idx = 0;
       Idx < cast<FixedVectorType>(Vector->getType())->getNumElements();
       ++Idx) {
    Arguments.push_back(B.CreateExtractElement(Vector, B.getInt32(Idx)));
  }

  return Arguments;
}

// Check if @Arg is of fixed vector type. If yes, flatten it into extractelement
// instructions and append them to @Arguments. Return true if @Arguments
// changed, false otherwise.
static bool flattenVectorArgument(IRBuilder<> &B, Value *Arg,
                                  SmallVectorImpl<Value *> &Arguments) {
  if (isa<FixedVectorType>(Arg->getType())) {
    const auto &FlattenedArguments = flattenVectorArgument(B, Arg);
    if (!FlattenedArguments.empty()) {
      Arguments.append(FlattenedArguments.begin(), FlattenedArguments.end());

      return true;
    }
  }

  return false;
}

/// Convert the called shader type to the ShaderKind of the called function.
DXILShaderKind LowerRaytracingPipelinePassImpl::callTypeToShaderKind(
    ContinuationCallType CallType) {
  switch (CallType) {
  case ContinuationCallType::AnyHit:
    return DXILShaderKind::AnyHit;
  case ContinuationCallType::CallShader:
    return DXILShaderKind::Callable;
  case ContinuationCallType::Traversal:
    return DXILShaderKind::RayGeneration;
  }
  llvm_unreachable("Unhandled enum value");
}

/// Clone a function and replace a call with a call to the cloned function
void LowerRaytracingPipelinePassImpl::replaceCall(
    FunctionData &Data, CallInst *Call, Function *Func,
    ContinuationCallType CallType) {
  Builder.SetInsertPoint(Call);
  auto *AfterCall = &*++Builder.GetInsertPoint();
  auto *SystemDataTy = getFuncArgPtrElementType(Func, 0);
  Value *PayloadOrAttrs = nullptr;

  SmallVector<Value *, 17> Arguments;
  Arguments.push_back(getDXILSystemData(Builder, Data.SystemData,
                                        Data.SystemDataTy,
                                        cast<StructType>(SystemDataTy)));

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
  Type *PayloadOrAttrsTy = DXILContHelper::getPayloadTypeFromMetadata(*Call);
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
        if (FuncName.starts_with("_AmdAwait") ||
            FuncName.starts_with("_AmdWaitAwait")) {
          AwaitCalls.push_back(CI);
        } else if (FuncName.starts_with("_AmdAcceptHitAttributes")) {
          AcceptHitAttrsCalls.push_back(CI);
        }
      }
    }
  }

  for (auto *CI : AwaitCalls) {
    Builder.SetInsertPoint(CI);
    replaceContinuationCall(CallType, CI, Data, PayloadOrAttrs,
                            PayloadOrAttrsTy);
  }

  for (auto *CI : AcceptHitAttrsCalls) {
    // Commit hit attributes
    Builder.SetInsertPoint(CI);
    assert(TraversalDataTy != 0 && "Missing traversal system data!");
    copyHitAttributes(Data, CI->getArgOperand(0), TraversalDataTy,
                      PayloadOrAttrs, false, nullptr);
    // Make sure that we store the hit attributes into the correct system
    // data (just in case dxc copied them around).
    assert(CI->getArgOperand(0) == Arguments[0] &&
           "AcceptHitAttributes does not take the correct system data as "
           "argument!");
    CI->eraseFromParent();
  }
  Builder.SetInsertPoint(AfterCall);
}

void LowerRaytracingPipelinePassImpl::handleRestoreSystemData(CallInst *Call) {
  // Store system data
  auto *SystemDataTy =
      cast<StructType>(getFuncArgPtrElementType(Call->getCalledFunction(), 0));
  auto *SystemData = Call->getArgOperand(0);

  // Set local root signature on re-entry
  assert(GetLocalRootIndex && "Could not find GetLocalRootIndex function");
  auto *LocalIndexSystemDataTy =
      cast<StructType>(getFuncArgPtrElementType(GetLocalRootIndex, 0));
  auto *LocalIndexSystemData = getDXILSystemData(
      Builder, SystemData, SystemDataTy, LocalIndexSystemDataTy);
  auto *LocalIndex =
      CrossInliner.inlineCall(Builder, GetLocalRootIndex, LocalIndexSystemData)
          .returnValue;
  LocalIndex->setName("local.root.index");
  Builder.CreateCall(SetLocalRootIndex, LocalIndex);
}

/// Replace a call to lgc.rt.report.hit with a call to the driver
/// implementation.
void LowerRaytracingPipelinePassImpl::replaceReportHitCall(FunctionData &Data,
                                                           CallInst *Call) {
  assert(ReportHit && "ReportHit not found");
  Function *F = Call->getFunction();

  replaceCall(Data, Call, ReportHit, ContinuationCallType::AnyHit);

  // Check if the search ended and return from Intersection if this is the case
  assert(IsEndSearch && "IsEndSearch not found");
  auto *SystemDataTy = getFuncArgPtrElementType(IsEndSearch, 0);
  auto *SystemData = getDXILSystemData(Builder, Data.SystemData,
                                       Data.SystemDataTy, SystemDataTy);
  auto *IsEnd =
      CrossInliner.inlineCall(Builder, IsEndSearch, SystemData).returnValue;
  Instruction *Then =
      SplitBlockAndInsertIfThen(IsEnd, &*Builder.GetInsertPoint(), true);
  Builder.SetInsertPoint(Then);
  SystemData = getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy,
                                 cast<StructType>(Data.ReturnTy));
  Value *RetSystemData = Builder.CreateLoad(Data.ReturnTy, SystemData);

  if (Mutator.shouldRun()) {
    uint32_t CpsRetLevel = getPotentialCpsReturnLevels(
        convertShaderKindToCpsShaderStage(Data.Kind));
    Builder.create<JumpOp>(
        F->getArg(ArgReturnAddr), CpsRetLevel,
        PoisonValue::get(StructType::get(Builder.getContext())), RetSystemData);
    Builder.CreateUnreachable();
  } else {
    auto *Ret = Builder.CreateRet(RetSystemData);

    // Assume worst-case payload size for Intersection. See the note on the
    // incoming payload size.
    DXILContHelper::setOutgoingRegisterCount(
        Ret, MetadataState.getMaxPayloadRegisterCount());
  }

  // Remove trailing unreachable
  Then->eraseFromParent();
}

/// Replace a call to Await with
/// - Allocate space for the passed payload
/// - Store payload into the global
/// - Call given address and pass generated token into an await call
/// - Read payload from global
void LowerRaytracingPipelinePassImpl::replaceContinuationCall(
    ContinuationCallType CallType, CallInst *Call, const FunctionData &Data,
    Value *PayloadOrAttrs, Type *PayloadOrAttrsTy) {
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
    PAQPayloadConfig PAQConfig = {PayloadOrAttrsTy,
                                  Data.FuncConfig.MaxHitAttributeBytes};
    if (CallType == ContinuationCallType::Traversal) {
      const auto *TraceRayInfo =
          &PAQManager.getOrCreateTraceRaySerializationInfo(PAQConfig);
      OutgoingSerializationInfo = TraceRayInfo;
      OutgoingSerializationLayout =
          &TraceRayInfo->LayoutsByKind[PAQSerializationLayoutKind::CallerOut];
      ShaderStage = PAQShaderStage::Caller;
      // determine ReturnedRegisterCount
      ReturnedRegisterCount = std::min(
          std::max(
              TraceRayInfo
                  ->LayoutsByKind[PAQSerializationLayoutKind::ClosestHitOut]
                  .NumStorageI32s,
              TraceRayInfo->LayoutsByKind[PAQSerializationLayoutKind::MissOut]
                  .NumStorageI32s),
          MetadataState.getMaxPayloadRegisterCount());

    } else {
      assert(CallType == ContinuationCallType::CallShader &&
             "Unexpected call type!");
      const auto *CallShaderInfo =
          &PAQManager.getOrCreateCallShaderSerializationInfo(PAQConfig);
      OutgoingSerializationLayout =
          &CallShaderInfo->CallShaderSerializationLayout;
      OutgoingSerializationInfo = CallShaderInfo;
      // For CallShader, incoming and outgoing layouts are the same
      ReturnedRegisterCount =
          std::min(MetadataState.getMaxPayloadRegisterCount(),
                   OutgoingSerializationLayout->NumStorageI32s);
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
      // If we have a mem pointer, then we need to allocate stack storage
      // The reverse does not hold, as a different payload type in the same
      // shader could require the allocation.
      assert((Data.PayloadSpillSize != 0) && "Inconsistent payload stack size");

      // Peek into the stack. This eventually will become lgc.cps.peek
      auto *CspType = getContinuationStackOffsetType(Builder.getContext());
      auto *CspPtr = Builder.CreateCall(getContinuationStackOffset(*Mod));
      auto *Csp = Builder.CreateLoad(CspType, CspPtr);
      Value *LocalPayloadMem =
          Builder.CreateAdd(Csp, Builder.getInt32(-Data.PayloadSpillSize));

#ifndef NDEBUG
      // Check that payload pointer exists and is in first position
      auto It = OutgoingSerializationLayout->NodeStorageInfos.find(
          OutgoingSerializationLayout->PayloadMemPointerNode);
      assert(It != OutgoingSerializationLayout->NodeStorageInfos.end() &&
             (It->second.IndexIntervals ==
              PAQIndexIntervals{{FirstPayloadMemoryPointerRegister,
                                 FirstPayloadMemoryPointerRegister + 1}}) &&
             "Payload memory pointer at unexpected location!");
#endif

      // Copy to payload storage
      Value *CastPayload = Builder.CreateBitCast(
          Payload, I32->getPointerTo(Payload->getAddressSpace()));
      Builder.CreateStore(LocalPayloadMem, CastPayload);
      // Barrier to ensure that accesses to the potentially in-memory parts of
      // the payload are not re-ordered before this store. More precisely, later
      // we will insert a load to the payload memory pointer at these accesses.
      // These loads must be after the store.
      Builder.CreateCall(RegisterBufferSetPointerBarrier, {Payload});
      // Set stacksize metadata on F
      setStacksizeMetadata(*Call->getFunction(), Data.PayloadSpillSize);
    }
    // Copy local payload to global payload, before await call (e.g. TraceRay,
    // CallShader)
    copyPayload(*PayloadOrAttrsTy, PayloadOrAttrs, ShaderStage,
                PAQAccessKind::Write, *OutgoingSerializationLayout);
  }

  auto *ShaderAddr = Call->getArgOperand(0);

  auto *FTy = Call->getFunctionType();
  SmallVector<Type *, 2> ArgTys;
  SmallVector<Value *, 2> Args;

  // Pass the given arguments, skipping the function address
  ArgTys.append(FTy->param_begin() + 1, FTy->param_end());
  Args.append(Call->arg_begin() + 1, Call->arg_end());

  auto *SystemDataTy = SetupRayGen->getReturnType();
  if (CallType == ContinuationCallType::AnyHit) {
    assert(TraversalDataTy && "Failed to detect traversal system data type");
    SystemDataTy = TraversalDataTy;
    // Add hit attributes to arguments
    ArgTys.push_back(PayloadOrAttrsTy);
    auto *HitAttrs = Builder.CreateLoad(PayloadOrAttrsTy, PayloadOrAttrs);
    Args.push_back(HitAttrs);
  }

  Value *NewCall = nullptr;
  if (Mutator.shouldRun()) {
    NewCall = Mutator.insertCpsAwait(
        Call->getType(), ShaderAddr, Call, Args, CallType,
        convertShaderKindToCpsShaderStage(Data.Kind));
  } else {
    auto *ShaderTy = FunctionType::get(TokenTy, ArgTys, false);
    auto *ShaderFun =
        Builder.CreateIntToPtr(ShaderAddr, ShaderTy->getPointerTo());

    auto *Token = Builder.CreateCall(ShaderTy, ShaderFun, Args);
    auto *Await =
        getContinuationAwait(*Mod, TokenTy, cast<StructType>(SystemDataTy));
    NewCall = Builder.CreateCall(Await, {Token});

    // Annotate call with the number of registers used for payload
    DXILContHelper::setOutgoingRegisterCount(
        Token, std::min(OutgoingSerializationLayout
                            ? OutgoingSerializationLayout->NumStorageI32s
                            : MetadataState.getMaxPayloadRegisterCount(),
                        MetadataState.getMaxPayloadRegisterCount()));
    DXILContHelper::setReturnedRegisterCount(Token,
                                             ReturnedRegisterCount.value());

    // For WaitAwait, add metadata indicating that we wait. After coroutine
    // passes, we then generate a waitContinue on the awaited function.
    if (Call->getCalledFunction()->getName().startswith("_AmdWaitAwait"))
      DXILContHelper::setIsWaitAwaitCall(*Token);
  }

  if (CallType != ContinuationCallType::AnyHit) {
    // Copy global payload back to local payload
    // Overwrite the local payload with poison first, to make sure it is not
    // seen as live state.
    Builder.CreateStore(PoisonValue::get(PayloadOrAttrsTy), PayloadOrAttrs);

    if (CallType == ContinuationCallType::CallShader) {
      // For CallShader, there is only a single layout
      // Copy global payload to local payload, after CallShader call
      copyPayload(*PayloadOrAttrsTy, PayloadOrAttrs, ShaderStage,
                  PAQAccessKind::Read, *OutgoingSerializationLayout);
    } else {
      copyTraceRayPayloadIncomingToCaller(
          *cast<PAQTraceRaySerializationInfo>(OutgoingSerializationInfo),
          PayloadOrAttrs);
    }
  }

  if (!Call->getType()->isVoidTy())
    Call->replaceAllUsesWith(NewCall);
  Call->eraseFromParent();
}

// If ReportHit is called for opaque geometry or if there is no AnyHit shader,
// ReportHit has to store the passed hit attributes to the payload global.
void LowerRaytracingPipelinePassImpl::handleReportHit(FunctionData &Data,
                                                      Function &F) {
  auto *HitAttrsArg = F.getArg(F.arg_size() - 1);

  // Look for accept hit calls
  for (auto &BB : F) {
    for (auto &I : make_early_inc_range(BB)) {
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        if (Call->getCalledFunction()->getName().starts_with(
                "_AmdAcceptHitAttributes")) {
          // Commit hit attributes
          Builder.SetInsertPoint(Call);
          assert(TraversalDataTy != 0 && "Missing traversal system data!");
          copyHitAttributes(Data, Call->getArgOperand(0), TraversalDataTy,
                            HitAttrsArg, false, nullptr);
          // Make sure that we store the hit attributes into the correct system
          // data (just in case dxc copied them around).
          assert(Call->getArgOperand(0) == F.getArg(0) &&
                 "AcceptHitAttributes does not take the correct system data as "
                 "argument!");
          Call->eraseFromParent();
        }
      }
    }
  }
}

/// Replace a call to lgc.rt.shader.index with the passed shader index argument.
void LowerRaytracingPipelinePassImpl::replaceShaderIndexCall(FunctionData &Data,
                                                             CallInst *Call) {
  if (Data.Kind == DXILShaderKind::RayGeneration) {
    Call->replaceAllUsesWith(Builder.getInt32(0));
  } else {
    auto *ShaderIndex = Call->getFunction()->getArg(ArgShaderIndex);
    Call->replaceAllUsesWith(ShaderIndex);
  }
  Call->eraseFromParent();
}

void LowerRaytracingPipelinePassImpl::handleGetFuncAddr(Function &Func) {
  assert(Func.arg_empty()
         // returns i64 or i32
         && (Func.getFunctionType()->getReturnType()->isIntegerTy(64) ||
             Func.getFunctionType()->getReturnType()->isIntegerTy(32)));

  auto Name = Func.getName();
  [[maybe_unused]] bool Consumed = Name.consume_front("_AmdGetFuncAddr");
  assert(Consumed);

  Constant *Addr = Mod->getFunction(Name);
  if (!Addr)
    report_fatal_error(Twine("Did not find function '") + Name +
                       "' requested by _AmdGetFuncAddr");
  Addr = ConstantExpr::getPtrToInt(Addr, Func.getReturnType());

  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    CInst.replaceAllUsesWith(Addr);
    CInst.eraseFromParent();
  });
}

void LowerRaytracingPipelinePassImpl::handleGetShaderKind(Function &Func) {
  assert(Func.getReturnType()->isIntegerTy(32) && Func.arg_size() == 0);

  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    Function *F = CInst.getFunction();
    auto Stage = lgc::rt::getLgcRtShaderStage(F);

    // Ignore GetShaderKind calls where we cannot find the shader kind.
    // This happens e.g. in gpurt-implemented intrinsics that got inlined,
    // but not removed.
    if (!Stage)
      return;

    DXILShaderKind ShaderKind =
        DXILContHelper::shaderStageToDxilShaderKind(*Stage);
    auto *ShaderKindVal = ConstantInt::get(Func.getReturnType(),
                                           static_cast<uint64_t>(ShaderKind));
    CInst.replaceAllUsesWith(ShaderKindVal);
    CInst.eraseFromParent();
  });
}

void LowerRaytracingPipelinePassImpl::handleGetCurrentFuncAddr(Function &Func) {
  assert(Func.arg_size() == 0 &&
         // Returns an i32 or i64
         (Func.getReturnType()->isIntegerTy(32) ||
          Func.getReturnType()->isIntegerTy(64)));

  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    auto *FuncPtrToInt =
        ConstantExpr::getPtrToInt(CInst.getFunction(), Func.getReturnType());
    CInst.replaceAllUsesWith(FuncPtrToInt);
    CInst.eraseFromParent();
  });
}

void llvm::copyBytes(IRBuilder<> &B, Value *Dst, Value *Src,
                     uint64_t NumBytes) {
  assert(Dst->getType()->isPointerTy() && Src->getType()->isPointerTy() &&
         "Dst and Src must be pointers!");
  auto *I32 = B.getInt32Ty();

  uint64_t NumFullI32s = NumBytes / RegisterBytes;
  // Copy full I32s
  for (uint64_t I32Index = 0; I32Index < NumFullI32s; ++I32Index) {
    auto *DstPtr = B.CreateConstGEP1_64(I32, Dst, I32Index);
    auto *SrcPtr = B.CreateConstGEP1_64(I32, Src, I32Index);
    auto *Val = B.CreateLoad(I32, SrcPtr);
    B.CreateStore(Val, DstPtr);
  }

  // Copy remaining bytes
  uint64_t NumRemainingBytes = NumBytes - (NumFullI32s * RegisterBytes);
  if (NumRemainingBytes == 0)
    return;

  // Create a packed struct containing NumRemainingBytes many i8, bitcast src
  // and dst pointers (+ offset) to the packed struct, and load/store the
  // struct. A similar technique is used in RegisterBufferPass.
  Type *I8 = B.getIntNTy(8);
  SmallVector<Type *, RegisterBytes> Elements;
  Elements.resize(NumRemainingBytes, I8);
  Type *StructTy = StructType::create(Elements, "", /* isPacked */ true);

  // Note: These pointers may not be dereferenced as I32s, because the I32s
  // overlap past the end of the Dst and Src range
  auto *DstStructPtr = B.CreateBitCast(
      B.CreateConstGEP1_64(I32, Dst, NumFullI32s),
      StructTy->getPointerTo(Dst->getType()->getPointerAddressSpace()));
  auto *SrcStructPtr = B.CreateBitCast(
      B.CreateConstGEP1_64(I32, Src, NumFullI32s),
      StructTy->getPointerTo(Src->getType()->getPointerAddressSpace()));

  auto *Val = B.CreateLoad(StructTy, SrcStructPtr);
  B.CreateStore(Val, DstStructPtr);
}

void LowerRaytracingPipelinePassImpl::copyPayload(
    Type &PayloadTy, Value *LocalPayload, std::optional<PAQShaderStage> Stage,
    PAQAccessKind GlobalAccessKind, const PAQSerializationLayout &Layout,
    SmallDenseSet<const PAQNode *, 16> *CopiedNodes) {
  // Nothing to do if there is no serialization type, i.e. the layout is empty
  if (!Layout.SerializationTy)
    return;

  // Obtain pointer to global payload serialization struct
  Value *PayloadSerialization = Builder.CreateBitCast(
      Payload,
      Layout.SerializationTy->getPointerTo(Payload->getAddressSpace()));

  PayloadCopyHelper Helper{
      *Mod,
      Builder,
      PayloadTy,
      LocalPayload,
      Stage,
      GlobalAccessKind,
      {Builder.getInt32(0)},
      CopiedNodes,
      PayloadSerialization,
      &Layout,
  };
  Helper.copyPayloadRecursively(Layout.PayloadRootNode);
}

void LowerRaytracingPipelinePassImpl::copyTraceRayPayloadIncomingToCaller(
    const PAQTraceRaySerializationInfo &SerializationInfo,
    Value *LocalPayload) {
  SmallDenseSet<const PAQNode *, 16> CopiedNodes;

  for (auto LayoutKind : {PAQSerializationLayoutKind::ClosestHitOut,
                          PAQSerializationLayoutKind::MissOut}) {
    const PAQSerializationLayout &Layout =
        SerializationInfo.LayoutsByKind[LayoutKind];
    copyPayload(*SerializationInfo.PayloadRootNode->Ty, LocalPayload,
                PAQShaderStage::Caller, PAQAccessKind::Read, Layout,
                &CopiedNodes);
  }
}

void LowerRaytracingPipelinePassImpl::savePayloadRegistersBeforeRecursion(
    DXILShaderKind Kind, const PAQSerializationLayout &IncomingLayout,
    const PAQSerializationLayout &OutgoingLayout,
    SmallVectorImpl<Value *> &SavedRegisterValues) {

  if (!OutgoingLayout.SerializationTy)
    return;

  SavedRegisterValues.resize(MetadataState.getMaxPayloadRegisterCount());

  std::optional<PAQShaderStage> Stage = dxilShaderKindToPAQShaderStage(Kind);
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
    assert(IncomingLayout.NodeStorageInfos.count(Node) &&
           "Unexpectedly dead node!");

    for (const PAQIndexInterval &Interval : StorageInfo.IndexIntervals) {
      for (unsigned I = Interval.Begin;
           I <
           std::min(Interval.End, MetadataState.getMaxPayloadRegisterCount());
           ++I) {
        // Create backup of the I-th payload register
        auto *LoadPtr =
            Builder.CreateConstGEP2_32(Payload->getValueType(), Payload, 0, I);
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

  assert((OutgoingLayout.PayloadMemPointerNode == nullptr ||
          SavedRegisterValues[FirstPayloadMemoryPointerRegister]) &&
         "Payload mem pointer missing from saved registers!");
}

void LowerRaytracingPipelinePassImpl::restorePayloadRegistersAfterRecursion(
    const SmallVectorImpl<Value *> &SavedRegisterValues) {
  for (unsigned I = 0; I < SavedRegisterValues.size(); ++I) {
    Value *OldValue = SavedRegisterValues[I];
    if (OldValue) {
      auto *StorePtr =
          Builder.CreateGEP(Payload->getValueType(), Payload,
                            {Builder.getInt32(0), Builder.getInt32(I)});
      Builder.CreateStore(SavedRegisterValues[I], StorePtr);
    }
  }
}

void LowerRaytracingPipelinePassImpl::copyHitAttributes(
    FunctionData &Data, Value *SystemDataPtr, Type *SystemDataPtrTy,
    Value *LocalHitAttributes, bool GlobalToLocal,
    const PAQSerializationLayout *Layout) {
  auto *InlineHitAttrsTy = GetTriangleHitAttributes->getReturnType();
  uint64_t InlineHitAttrsBytes = getInlineHitAttrsBytes(*GpurtLibrary);
  uint64_t InlineRegSize = InlineHitAttrsBytes / RegisterBytes;
  auto *RegTy = Builder.getIntNTy(RegisterBytes * 8);
  auto *RegTyPtr = RegTy->getPointerTo();

  // Hit attribute storage is split between inline hit attributes in system
  // data, and possibly some payload registers. In order to access inline hit
  // attributes in the same way as payload registers (modeled as global i32
  // array), we add an alloca for inline hit attributes, copy from system data
  // to the alloca at the start, or copy back from the alloca to system data,
  // depending on GlobalToLocal. Then, in the actual copy implementation, we
  // just access the alloca using loads and stores as for payload registers.
  auto InsertPoint = Builder.saveIP();
  Builder.SetInsertPoint(
      Builder.GetInsertBlock()->getParent()->getEntryBlock().getFirstNonPHI());
  auto *InlineHitAttrsAlloc = Builder.CreateAlloca(InlineHitAttrsTy);
  Builder.restoreIP(InsertPoint);
  auto *InlineHitAttrs = Builder.CreateBitCast(InlineHitAttrsAlloc, RegTyPtr);

  if (GlobalToLocal) {
    // Load inline hit attributes from system data
    auto *SystemDataTy =
        cast<StructType>(getFuncArgPtrElementType(GetTriangleHitAttributes, 0));
    auto *SystemData = getDXILSystemData(Builder, SystemDataPtr,
                                         SystemDataPtrTy, SystemDataTy);
    auto *InlineHitAttrs =
        CrossInliner.inlineCall(Builder, GetTriangleHitAttributes, SystemData)
            .returnValue;
    Builder.CreateStore(InlineHitAttrs, InlineHitAttrsAlloc);
  }

  // Hit attribute storage in payload storage
  Value *PayloadHitAttrs = nullptr;
  [[maybe_unused]] unsigned PayloadHitAttrBytes = 0;

  // Find hit attributes in layout if present
  if (Layout) {
    if (Layout->HitAttributeStorageNode) {
      auto It = Layout->NodeStorageInfos.find(Layout->HitAttributeStorageNode);
      assert(It != Layout->NodeStorageInfos.end() &&
             "Missing hit attributes in layout!");
      const PAQIndexIntervals &IndexIntervals = It->second.IndexIntervals;
      assert(IndexIntervals.size() == 1 &&
             "Hit attributes must be contiguous!");
      const PAQIndexInterval &IndexInterval = IndexIntervals[0];

      // Obtain pointer to global payload serialization struct
      Value *PayloadSerialization = Builder.CreateBitCast(
          Payload,
          Layout->SerializationTy->getPointerTo(Payload->getAddressSpace()));
      // Last zero yields pointer to the first element of the i32 array
      PayloadHitAttrs = Builder.CreateInBoundsGEP(
          Layout->SerializationTy, PayloadSerialization,
          {Builder.getInt32(0), Builder.getInt32(0),
           Builder.getInt32(IndexInterval.Begin)});
      PayloadHitAttrBytes = RegisterBytes * IndexInterval.size();
    } else {
      // Inline attributes suffice, nothing to do.
    }
  } else {
    assert(Data.Kind == DXILShaderKind::Intersection &&
           "Unexpected shader kind");
    // We are in an intersection shader, which does not know the payload type.
    // Assume maximum possible size
    PayloadHitAttrBytes =
        Data.FuncConfig.MaxHitAttributeBytes - InlineHitAttrsBytes;
    // Use hit attribute storage at fixed index
    PayloadHitAttrs =
        Builder.CreateConstGEP2_32(Payload->getValueType(), Payload, 0,
                                   FirstPayloadHitAttributeStorageRegister);
  }

  uint64_t HitAttrsBytes =
      DL->getTypeStoreSize(Data.HitAttributes).getFixedValue();
  if (HitAttrsBytes > Data.FuncConfig.MaxHitAttributeBytes)
    report_fatal_error("Hit attributes are too large!");
  assert(InlineHitAttrsBytes + PayloadHitAttrBytes >= HitAttrsBytes &&
         "Insufficient hit attribute storage!");
  LocalHitAttributes = Builder.CreateBitCast(LocalHitAttributes, RegTyPtr);
  auto *I8Ty = Builder.getInt8Ty();
  for (unsigned I = 0; I < divideCeil(HitAttrsBytes, RegisterBytes); I++) {
    auto *LocalPtr =
        Builder.CreateConstInBoundsGEP1_64(RegTy, LocalHitAttributes, I);
    Value *GlobalPtr;
    if (I < InlineRegSize)
      GlobalPtr = Builder.CreateConstInBoundsGEP1_64(RegTy, InlineHitAttrs, I);
    else
      GlobalPtr = Builder.CreateConstInBoundsGEP1_64(RegTy, PayloadHitAttrs,
                                                     I - InlineRegSize);

    auto *LoadPtr = GlobalToLocal ? GlobalPtr : LocalPtr;
    auto *StorePtr = GlobalToLocal ? LocalPtr : GlobalPtr;
    if ((I + 1) * RegisterBytes <= HitAttrsBytes) {
      // Can load a whole register
      auto *Val = Builder.CreateLoad(RegTy, LoadPtr);
      Builder.CreateStore(Val, StorePtr);
    } else {
      // Load byte by byte into a vector and pad the rest with undef
      auto *ByteLoadPtr = Builder.CreateBitCast(LoadPtr, I8Ty->getPointerTo());
      auto *ByteStorePtr =
          Builder.CreateBitCast(StorePtr, I8Ty->getPointerTo());
      for (unsigned J = 0; J < HitAttrsBytes % RegisterBytes; J++) {
        auto *Val = Builder.CreateLoad(
            I8Ty, Builder.CreateConstInBoundsGEP1_64(I8Ty, ByteLoadPtr, J));
        Builder.CreateStore(
            Val, Builder.CreateConstInBoundsGEP1_64(I8Ty, ByteStorePtr, J));
      }
    }
  }

  if (!GlobalToLocal) {
    // Store inline hit attributes to system data
    auto *Attrs = Builder.CreateLoad(InlineHitAttrsTy, InlineHitAttrsAlloc);
    auto *SystemDataTy =
        cast<StructType>(getFuncArgPtrElementType(GetTriangleHitAttributes, 0));
    auto *SystemData = getDXILSystemData(Builder, SystemDataPtr,
                                         SystemDataPtrTy, SystemDataTy);
    assert(SetTriangleHitAttributes &&
           "Could not find SetTriangleHitAttributes function");
    CrossInliner.inlineCall(Builder, SetTriangleHitAttributes,
                            {SystemData, Attrs});
  }
}

void LowerRaytracingPipelinePassImpl::createPayloadGlobal() {
  I32 = Type::getInt32Ty(*Context);

  // Find maximum payload storage size:
  // If there is a set minimum payload register count, rely on that value being
  // large enough to ensure shaders in this module are compatible with other
  // shaders they are going to be used with. Otherwise, use the maximum allowed
  // number of payload registers (this is by default assigned to
  // MinPayloadRegisterCount, if MinRegisterCount is not set on the module
  // metadata.) Note: this influences the payload size in Traversal.
  uint32_t MaxPayloadI32s = MetadataState.getMinPayloadRegisterCount();
  for (const auto &[_, FuncData] : ToProcess) {
    MaxPayloadI32s = std::max(MaxPayloadI32s, FuncData.MaxOutgoingPayloadI32s);
    if (FuncData.IncomingPayloadSerializationInfo)
      MaxPayloadI32s =
          std::max(MaxPayloadI32s,
                   FuncData.IncomingPayloadSerializationInfo->MaxStorageI32s);
  }
  auto *PayloadTy = ArrayType::get(I32, MaxPayloadI32s);

  Payload = cast<GlobalVariable>(
      Mod->getOrInsertGlobal(DXILContHelper::GlobalPayloadName, PayloadTy, [&] {
        auto *Payload = new GlobalVariable(
            *Mod, PayloadTy, false, GlobalVariable::ExternalLinkage, nullptr,
            DXILContHelper::GlobalPayloadName, nullptr,
            GlobalVariable::NotThreadLocal);

        // Add registerbuffer metadata unconditionally to split all accesses
        // into i32s
        RegisterBufferMD RMD;
        RMD.RegisterCount = MetadataState.getMaxPayloadRegisterCount();
        RMD.Addrspace =
            static_cast<uint32_t>(MetadataState.getContStackAddrspace());
        auto *MD = createRegisterBufferMetadata(*Context, RMD);
        Payload->addMetadata("registerbuffer", *MD);

        return Payload;
      }));
}

void LowerRaytracingPipelinePassImpl::setTraversalRegisterCountMetadata() {
  const uint32_t NumI32s = std::min(
      static_cast<uint32_t>(Payload->getValueType()->getArrayNumElements()),
      MetadataState.getMaxPayloadRegisterCount());

  // Find traversal functions without walking over all functions by checking
  // uses of the `continuation.[wait]continue` intrinsics.
  for (const auto &Name :
       {"continuation.continue", "continuation.waitContinue"}) {
    auto *Func = Mod->getFunction(Name);
    if (!Func)
      continue;
    for (auto *User : Func->users()) {
      CallInst *CI = dyn_cast<CallInst>(User);
      if (!isa<CallInst>(User) || CI->getCalledFunction() != Func)
        continue;

      auto *TraversalVariant = CI->getFunction();
      auto Stage = lgc::rt::getLgcRtShaderStage(TraversalVariant);
      if (!Stage || *Stage != lgc::rt::RayTracingShaderStage::Traversal)
        continue;

      assert(!DXILContHelper::tryGetOutgoingRegisterCount(CI).has_value() &&
             "Unexpected register count metadata");
      DXILContHelper::setOutgoingRegisterCount(CI, NumI32s);

      assert(DXILContHelper::tryGetIncomingRegisterCount(TraversalVariant)
                     .value_or(NumI32s) == NumI32s &&
             "Unexpected incoming register count on Traversal");
      DXILContHelper::setIncomingRegisterCount(TraversalVariant, NumI32s);
    }
  }
}

void LowerRaytracingPipelinePassImpl::processContinuations() {
  TokenTy = StructType::create(*Context, "continuation.token")->getPointerTo();
  RegisterBufferSetPointerBarrier = getRegisterBufferSetPointerBarrier(*Mod);

  for (auto &FuncData : ToProcess) {
    processFunction(FuncData.first, FuncData.second);
  }
}

void LowerRaytracingPipelinePassImpl::processFunctionEntry(Function *F,
                                                           FunctionData &Data) {
  // Create system data
  // See also the system data documentation at the top of Continuations.h.
  Data.SystemData = Builder.CreateAlloca(Data.SystemDataTy);
  Data.SystemData->setName("system.data.alloca");
  // Initialize system data by calling the getSystemData intrinsic
  auto *SystemDataIntr =
      Builder.create<continuations::GetSystemDataOp>(Data.SystemDataTy);
  Builder.CreateStore(SystemDataIntr, Data.SystemData);

  // Set local root signature on entry
  assert(GetLocalRootIndex && "Could not find GetLocalRootIndex function");
  auto *LocalIndex =
      CrossInliner
          .inlineCall(
              Builder, GetLocalRootIndex,
              getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy,
                                getFuncArgPtrElementType(GetLocalRootIndex, 0)))
          .returnValue;
  LocalIndex->setName("local.root.index");
  Builder.CreateCall(SetLocalRootIndex, LocalIndex);

  // Allocate payload spilling space
  if (Data.PayloadSpillSize > 0)
    moveContinuationStackOffset(Builder, Data.PayloadSpillSize);
}

void LowerRaytracingPipelinePassImpl::processFunctionEnd(
    FunctionData &Data, FunctionEndData &EData) {
  AnyHitExitKind AHExitKind = AnyHitExitKind::None;
  bool IsAnyHit = Data.Kind == DXILShaderKind::AnyHit;

  if (IsAnyHit) {
    // Default to AcceptHit, which is only implicitly represented by
    // the absence of a call to the other intrinsics.
    AHExitKind = AnyHitExitKind::AcceptHit;
    // Search backwards from the terminator to find a call to one of
    // acceptHitAndEndSearch or ignoreHit.
    if (EData.Terminator != EData.Terminator->getParent()->getFirstNonPHI()) {
      auto Before = --EData.Terminator->getIterator();
      if (auto *Call = dyn_cast<CallInst>(Before)) {
        if (auto *Callee = Call->getCalledFunction()) {
          if (isa<AcceptHitAndEndSearchOp>(Call))
            AHExitKind = AnyHitExitKind::AcceptHitAndEndSearch;
          else if (isa<IgnoreHitOp>(Call))
            AHExitKind = AnyHitExitKind::IgnoreHit;
        }
      }
    }
  }

  Builder.SetInsertPoint(EData.Terminator);

  auto *PayloadTy = Data.IncomingPayload;
  if (Data.Kind != DXILShaderKind::RayGeneration &&
      Data.Kind != DXILShaderKind::Intersection) {
    assert(PayloadTy && "Missing payload type!");

    if (IsAnyHit) {
      if (AHExitKind == AnyHitExitKind::AcceptHit) {
        // Add a call to AcceptHit
        assert(AcceptHit && "Could not find AcceptHit function");
        auto *SystemDataTy =
            cast<StructType>(getFuncArgPtrElementType(AcceptHit, 0));
        auto *SystemData = getDXILSystemData(Builder, Data.SystemData,
                                             Data.SystemDataTy, SystemDataTy);
        CrossInliner.inlineCall(Builder, AcceptHit, SystemData);
      }

      EData.OutgoingSerializationLayout =
          &PAQManager.getOrCreateShaderExitSerializationLayout(
              *Data.IncomingPayloadSerializationInfo, Data.Kind,
              Data.HitAttributes, AHExitKind);
    }
    assert(EData.OutgoingSerializationLayout && "Missing layout");

    // Restore saved registers. This needs to be done *before* copying
    // back the payload, which depends on the restored memory pointer!
    restorePayloadRegistersAfterRecursion(EData.SavedRegisterValues);

    // Copy local payload into global payload at end of shader
    if (EData.OutgoingSerializationLayout->NumStorageI32s) {
      Builder.CreateCall(RegisterBufferSetPointerBarrier, {Payload});
      copyPayload(*PayloadTy, EData.NewPayload, EData.ShaderStage,
                  PAQAccessKind::Write, *EData.OutgoingSerializationLayout);
    }

    if (IsAnyHit) {
      // Copy hit attributes into payload for closest hit
      if (AHExitKind == AnyHitExitKind::AcceptHit ||
          AHExitKind == AnyHitExitKind::AcceptHitAndEndSearch) {
        // TODO Only if there is a ClosestHit shader in any hit group
        // where this AnyHit is used. If there is no ClosestHit, the
        // attributes can never be read, so we don't need to store them.
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy,
                          EData.HitAttrsAlloca, false,
                          EData.OutgoingSerializationLayout);
      } else {
        assert(AHExitKind == AnyHitExitKind::IgnoreHit);
        // Copy original hit attributes
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy,
                          EData.OrigHitAttrsAlloca, false,
                          EData.OutgoingSerializationLayout);
      }
    }
  }

  if (Data.PayloadSpillSize > 0)
    moveContinuationStackOffset(Builder, -Data.PayloadSpillSize);

  Value *RetValue = nullptr;
  if (!Data.ReturnTy->isVoidTy()) {
    auto *SystemData =
        getDXILSystemData(Builder, Data.SystemData, Data.SystemDataTy,
                          cast<StructType>(Data.ReturnTy));
    RetValue = Builder.CreateLoad(Data.ReturnTy, SystemData);
  }

  if (Mutator.shouldRun()) {
    uint32_t CpsRetLevel = getPotentialCpsReturnLevels(
        convertShaderKindToCpsShaderStage(Data.Kind));
    SmallVector<Value *> RetArgs;

    if (RetValue)
      RetArgs.push_back(RetValue);

    if (Data.Kind == DXILShaderKind::RayGeneration) {
      assert(RetArgs.empty() && "RayGen cannot return anything");
      Builder.CreateRetVoid();
    } else {
      Builder.create<JumpOp>(
          EData.Terminator->getFunction()->getArg(ArgReturnAddr), CpsRetLevel,
          PoisonValue::get(StructType::get(Builder.getContext())), RetArgs);
      Builder.CreateUnreachable();
    }
  } else {
    Instruction *Ret =
        RetValue ? Builder.CreateRet(RetValue) : Builder.CreateRetVoid();

    // Annotate ret with number of outgoing payload registers.
    // This annotation will be passed along the following transformations,
    // ending up at the final continuation call.
    unsigned OutgoingRegisterCount =
        EData.OutgoingSerializationLayout
            ? std::min(EData.OutgoingSerializationLayout->NumStorageI32s,
                       MetadataState.getMaxPayloadRegisterCount())
            : MetadataState.getMaxPayloadRegisterCount();
    DXILContHelper::setOutgoingRegisterCount(Ret, OutgoingRegisterCount);
  }

  EData.Terminator->eraseFromParent();
}

void LowerRaytracingPipelinePassImpl::processFunction(Function *F,
                                                      FunctionData &Data) {
  Builder.SetInsertPointPastAllocas(F);

  // Change the return type and arguments for shaders that are not RayGen
  SmallVector<Type *> AllArgTypes;
  Type *NewRetTy;
  Type *SystemDataTy = nullptr;

  if (Mutator.shouldRun()) {
    // Create the CPS function header.

    // A CPS function signature consists of:
    //  * State: {}
    //  * Return continuation reference (RCR): i32
    //  * Shader index
    //  * Remaining arguments (system data, optionally hit attributes)

    AllArgTypes.push_back(StructType::get(Mod->getContext()));
    AllArgTypes.push_back(Builder.getInt32Ty());
    AllArgTypes.push_back(Builder.getInt32Ty());
  }

  if (Data.Kind == DXILShaderKind::RayGeneration) {
    assert(SetupRayGen && "Could not find SetupRayGen function");
    SystemDataTy = SetupRayGen->getReturnType();
    NewRetTy = Builder.getVoidTy();
  } else {
    switch (Data.Kind) {
    case DXILShaderKind::Intersection: {
      assert(TraversalDataTy && "Failed to detect traversal system data type");
      SystemDataTy = TraversalDataTy;
      AllArgTypes.push_back(SystemDataTy);
      NewRetTy = SystemDataTy;
      break;
    }
    case DXILShaderKind::AnyHit: {
      assert(TraversalDataTy && "Failed to detect traversal system data type");
      SystemDataTy = TraversalDataTy;
      AllArgTypes.push_back(SystemDataTy);
      AllArgTypes.push_back(Data.HitAttributes);
      NewRetTy = SystemDataTy;
      break;
    }
    case DXILShaderKind::ClosestHit:
    case DXILShaderKind::Miss: {
      assert(HitMissDataTy && "Failed to detect hit/miss system data type");
      assert(SetupRayGen && "Could not find SetupRayGen function");
      SystemDataTy = HitMissDataTy;
      AllArgTypes.push_back(SystemDataTy);
      NewRetTy = SetupRayGen->getReturnType();
      break;
    }
    case DXILShaderKind::Callable: {
      assert(SetupRayGen && "Could not find SetupRayGen function");
      SystemDataTy = SetupRayGen->getReturnType();
      AllArgTypes.push_back(SystemDataTy);
      NewRetTy = SystemDataTy;
      break;
    }
    default:
      llvm_unreachable("Unhandled ShaderKind");
    }
  }

  Data.PayloadSpillSize = computeNeededStackSizeForRegisterBuffer(
      Data.MaxOutgoingPayloadI32s, MetadataState.getMaxPayloadRegisterCount());
  assert((Data.PayloadSpillSize == 0) ||
         (Data.Kind != DXILShaderKind::Intersection));
  Data.SystemDataTy = cast<StructType>(SystemDataTy);
  processFunctionEntry(F, Data);

  auto *FunctionTypeRetTy =
      Mutator.shouldRun() ? Builder.getVoidTy() : NewRetTy;
  // Create new function to change signature
  auto *NewFuncTy = FunctionType::get(FunctionTypeRetTy, AllArgTypes, false);
  Function *NewFunc = CompilerUtils::cloneFunctionHeader(
      *F, NewFuncTy, ArrayRef<AttributeSet>{});
  NewFunc->takeName(F);

  llvm::moveFunctionBody(*F, *NewFunc);

  if (Mutator.shouldRun()) {
    NewFunc->getArg(ArgContState)->setName("cont.state");
    NewFunc->getArg(ArgReturnAddr)->setName("return.addr");
    NewFunc->getArg(ArgShaderIndex)->setName("shader.index");

    // Mark as CPS function with the corresponding level.
    CpsLevel Level =
        getCpsLevelForShaderStage(convertShaderKindToCpsShaderStage(Data.Kind));
    setCpsFunctionLevel(*NewFunc, Level);
  }

  FunctionEndData EData;
  if (Data.Kind == DXILShaderKind::RayGeneration) {
    if (!Mutator.shouldRun()) {
      NewFunc->setMetadata(DXILContHelper::MDEntryName,
                           MDTuple::get(*Context, {}));

      // Entry functions have no incoming payload or continuation state
      DXILContHelper::setIncomingRegisterCount(NewFunc, 0);
    }
  } else {
    // Ignore payload for intersection shaders, they don't touch payload
    Value *NewPayload = nullptr;
    // Hit attributes stored in payload at entry of any hit
    Value *OrigHitAttrsAlloca = nullptr;
    // Hit attributes passed to any hit as argument
    Value *HitAttrsAlloca = nullptr;

    Type *PayloadTy = Data.IncomingPayload;
    std::optional<PAQShaderStage> ShaderStage =
        dxilShaderKindToPAQShaderStage(Data.Kind);
    PAQSerializationInfoBase *SerializationInfo =
        Data.IncomingPayloadSerializationInfo;

    // For ClosestHit and Miss, we need to determine the out layout
    // early on in order to determine which payload fields to save in case of
    // recursive TraceRay / CallShader.
    const PAQSerializationLayout *OutgoingSerializationLayout = nullptr;
    // Maps indices of payload registers to the saved values (across a
    // recursive TraceRay or CallShader)
    SmallVector<Value *, 32> SavedRegisterValues{};

    if (Data.Kind != DXILShaderKind::Intersection) {
      assert(PayloadTy && "Missing payload type!");

      // For AnyHit, the layout depends on whether we accept or ignore, which
      // we do not know yet. In that case, the layout is determined later.
      if (Data.Kind != DXILShaderKind::AnyHit) {
        OutgoingSerializationLayout =
            &PAQManager.getOrCreateShaderExitSerializationLayout(
                *SerializationInfo, Data.Kind, Data.HitAttributes,
                AnyHitExitKind::None);
      }

      const PAQSerializationLayout &IncomingSerializationLayout =
          PAQManager.getOrCreateShaderStartSerializationLayout(
              *SerializationInfo, Data.Kind, Data.HitAttributes);
      // Handle reading global payload
      auto *FPayload = F->getArg(0);

      {
        // Preserve current insert point
        IRBuilder<>::InsertPointGuard Guard(Builder);
        Builder.SetInsertPointPastAllocas(NewFunc);
        NewPayload = Builder.CreateAlloca(PayloadTy);
        FPayload->replaceAllUsesWith(NewPayload);
      }

      if (Mutator.shouldRun()) {
        // TODO Read payload argument for lgc continuations
      } else {
        // Annotate function with the number of registers for incoming payload
        DXILContHelper::setIncomingRegisterCount(
            NewFunc, std::min(IncomingSerializationLayout.NumStorageI32s,
                              MetadataState.getMaxPayloadRegisterCount()));

        // Copy global payload into local payload at start of shader
        if (IncomingSerializationLayout.NumStorageI32s) {
          copyPayload(*PayloadTy, NewPayload, ShaderStage, PAQAccessKind::Read,
                      IncomingSerializationLayout);
          // Add barrier so no stores that may overwrite the memory pointer
          // are put before the payload is read
          Builder.CreateCall(RegisterBufferSetPointerBarrier, {Payload});
        }

        if (!Data.CallShaderCalls.empty() || !Data.TraceRayCalls.empty()) {
          assert(OutgoingSerializationLayout &&
                 "Missing outgoing serialization layout!");
          savePayloadRegistersBeforeRecursion(
              Data.Kind, IncomingSerializationLayout,
              *OutgoingSerializationLayout, SavedRegisterValues);
        }
      }

      // Handle hit attributes
      if (Data.Kind == DXILShaderKind::AnyHit) {
        assert(F->arg_size() == 2 && "Shader has more arguments than expected");
        auto *HitAttrs = F->getArg(1);

        {
          // Preserve current insert point
          IRBuilder<>::InsertPointGuard Guard(Builder);
          Builder.SetInsertPointPastAllocas(NewFunc);
          OrigHitAttrsAlloca = Builder.CreateAlloca(ArrayType::get(
              I32, divideCeil(GlobalMaxHitAttributeBytes, RegisterBytes)));
          OrigHitAttrsAlloca->setName("OrigHitAttrs");

          HitAttrsAlloca = Builder.CreateAlloca(Data.HitAttributes);
          HitAttrsAlloca->setName("HitAttrsAlloca");
        }

        // Copy old hit attributes from payload
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy,
                          OrigHitAttrsAlloca, true,
                          &IncomingSerializationLayout);

        // Copy new hit attributes from argument
        Builder.CreateStore(NewFunc->getArg(NewFunc->arg_size() - 1),
                            HitAttrsAlloca);
        HitAttrs->replaceAllUsesWith(HitAttrsAlloca);
      } else if (Data.Kind == DXILShaderKind::ClosestHit) {
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
        copyHitAttributes(Data, Data.SystemData, Data.SystemDataTy, NewHitAttrs,
                          true, &IncomingSerializationLayout);
      }
    } else {
      if (!Mutator.shouldRun()) {
        // Annotate intersection shader with the maximum number of registers
        // used for payload
        // TODO: When compiling a pipeline and not a library, we could figure
        //       out the pipeline-wide max (on a higher level than here) and use
        //       that instead. For a library compile, we can't know the max
        //       payload size of shaders in pipelines this shader is used in.
        DXILContHelper::setIncomingRegisterCount(
            NewFunc, MetadataState.getMaxPayloadRegisterCount());
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
  SmallVector<BasicBlock *> BBs(make_pointer_range(*NewFunc));
  for (auto *BB : BBs) {
    auto *I = BB->getTerminator();
    assert(I && "BB must have terminator");
    // Replace the end of the BB if it terminates the function
    bool IsFunctionEnd = (I->getOpcode() == Instruction::Ret ||
                          I->getOpcode() == Instruction::Unreachable);
    if (IsFunctionEnd) {
      EData.Terminator = I;
      processFunctionEnd(Data, EData);
    }
  }

  // Remove the old function
  F->replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F->getType()));
  F->eraseFromParent();
  F = NewFunc;

  MDTuple *ContMDTuple = MDTuple::get(*Context, {ValueAsMetadata::get(F)});
  F->setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);

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
  for (auto *Call : Data.ShaderIndexCalls) {
    Builder.SetInsertPoint(&*++Call->getIterator());
    replaceShaderIndexCall(Data, Call);
  }

  // Replace non-rematerializable intrinsic calls
  for (auto *Call : Data.IntrinsicCalls)
    replaceIntrinsicCall(Builder, Data.SystemDataTy, Data.SystemData, Data.Kind,
                         Call, GpurtLibrary, CrossInliner);

#ifndef NDEBUG
  if (!Mutator.shouldRun() && Data.Kind != DXILShaderKind::RayGeneration) {
    // Check that all returns have registercount metadata
    for (const auto &BB : *F) {
      auto *Terminator = BB.getTerminator();
      if (Terminator->getOpcode() == Instruction::Ret &&
          !DXILContHelper::tryGetOutgoingRegisterCount(Terminator))
        report_fatal_error("Missing registercount metadata!");
    }
  }
#endif
}

static uint32_t getMaxHitAttributeByteCount(const Function &F) {
  // Use max hit attribute size from metadata, or use globally max allowed
  // value for the max if metadata is not set
  uint32_t Result = DXILContHelper::tryGetMaxHitAttributeByteCount(F).value_or(
      GlobalMaxHitAttributeBytes);
  if (Result % RegisterBytes != 0) {
    auto AlignedSize = alignTo(Result, RegisterBytes);
    LLVM_DEBUG(dbgs() << "Aligning misaligned max hit attribute size " << Result
                      << " to " << AlignedSize << "\n");
    Result = AlignedSize;
  }
  return Result;
}

void LowerRaytracingPipelinePassImpl::collectProcessableFunctions() {
  for (auto &Func : *Mod) {
    auto Stage = lgc::rt::getLgcRtShaderStage(&Func);
    if (!Stage || Func.isDeclaration())
      continue;

    DXILShaderKind Kind = DXILContHelper::shaderStageToDxilShaderKind(*Stage);
    switch (Kind) {
    case DXILShaderKind::RayGeneration:
    case DXILShaderKind::Intersection:
    case DXILShaderKind::AnyHit:
    case DXILShaderKind::ClosestHit:
    case DXILShaderKind::Miss:
    case DXILShaderKind::Callable: {
      FunctionData Data;
      Data.Kind = Kind;
      Data.FuncConfig.MaxHitAttributeBytes = getMaxHitAttributeByteCount(Func);
      LLVM_DEBUG(dbgs() << "Shader " << Func.getName()
                        << " uses max hit attribute size of "
                        << Data.FuncConfig.MaxHitAttributeBytes << "\n");
      if (Kind != DXILShaderKind::Intersection &&
          Kind != DXILShaderKind::RayGeneration) {
        assert(!Func.arg_empty() && "Shader must have at least one argument");
        Data.IncomingPayload = getFuncArgPtrElementType(&Func, 0);
        PAQPayloadConfig PAQConfig = {Data.IncomingPayload,
                                      Data.FuncConfig.MaxHitAttributeBytes};
        Data.IncomingPayloadSerializationInfo =
            &PAQManager.getOrCreateSerializationInfo(PAQConfig, Kind);
        assert(Data.IncomingPayloadSerializationInfo != nullptr &&
               "Missing serialization info!");
      }
      if (Kind == DXILShaderKind::AnyHit ||
          Kind == DXILShaderKind::ClosestHit) {
        assert(Func.arg_size() >= 2 &&
               "Shader must have at least two arguments");
        Data.HitAttributes =
            getFuncArgPtrElementType(&Func, Func.arg_size() - 1);
      }

      if (Kind == DXILShaderKind::Intersection) {
        Data.MaxOutgoingPayloadI32s =
            MetadataState.getMaxPayloadRegisterCount();
      }

      ToProcess[&Func] = Data;
      break;
    }
    default:
      break;
    }
  }
}

// Assert that the types of the different driver functions are as expected
void LowerRaytracingPipelinePassImpl::handleDriverFuncAssertions() {
  if (IsEndSearch)
    assert(IsEndSearch->getReturnType() == Type::getInt1Ty(*Context) &&
           IsEndSearch->arg_size() == 1
           // Traversal data
           && IsEndSearch->getFunctionType()->getParamType(0)->isPointerTy());

  if (GetTriangleHitAttributes)
    assert(GetTriangleHitAttributes->getReturnType()
               ->isStructTy() // BuiltinTriangleIntersectionAttributes
           && GetTriangleHitAttributes->arg_size() == 1
           // System data
           && GetTriangleHitAttributes->getFunctionType()
                  ->getParamType(0)
                  ->isPointerTy());

  if (SetTriangleHitAttributes)
    assert(SetTriangleHitAttributes->getReturnType()->isVoidTy() &&
           SetTriangleHitAttributes->arg_size() == 2
           // System data
           && SetTriangleHitAttributes->getFunctionType()
                  ->getParamType(0)
                  ->isPointerTy()
           // BuiltinTriangleIntersectionAttributes
           && SetTriangleHitAttributes->getFunctionType()
                  ->getParamType(1)
                  ->isStructTy());

  if (GetLocalRootIndex)
    assert(
        GetLocalRootIndex->getReturnType() ==
            Type::getInt32Ty(Mod->getContext()) &&
        GetLocalRootIndex->arg_size() == 1
        // Dispatch data
        &&
        GetLocalRootIndex->getFunctionType()->getParamType(0)->isPointerTy());

  if (SetupRayGen)
    assert(SetupRayGen->getReturnType()->isStructTy() &&
           SetupRayGen->arg_empty());

  if (TraceRay)
    assert(TraceRay->getReturnType()->isVoidTy() &&
           TraceRay->arg_size() == 15
           // Dispatch data
           && TraceRay->getFunctionType()->getParamType(0)->isPointerTy());

  if (CallShader)
    assert(CallShader->getReturnType()->isVoidTy() &&
           CallShader->arg_size() == 2
           // Dispatch data
           && CallShader->getFunctionType()->getParamType(0)->isPointerTy()
           // Shader id
           && CallShader->getFunctionType()->getParamType(1) ==
                  Type::getInt32Ty(*Context));

  if (ReportHit)
    assert(ReportHit->getReturnType()->isIntegerTy(1) &&
           ReportHit->arg_size() == 3
           // Traversal data
           && ReportHit->getFunctionType()->getParamType(0)->isPointerTy());

  if (AcceptHit)
    assert(AcceptHit->getReturnType()->isVoidTy() &&
           AcceptHit->arg_size() == 1
           // Traversal data
           && AcceptHit->getFunctionType()->getParamType(0)->isPointerTy());
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
    handleGetFuncAddr(Func);
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
    if (F.getName().startswith("_AmdRestoreSystemData")) {
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
    if (!DialectUtils::isLgcRtOp(&Func))
      continue;

    static const llvm_dialects::OpSet NonRematerializableDialectOps =
        llvm_dialects::OpSet::get<TraceRayOp, ReportHitOp, CallCallableShaderOp,
                                  ShaderIndexOp>();
    if (!NonRematerializableDialectOps.contains(Func)) {
      llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
        auto Data = ToProcess.find(CInst.getFunction());
        if (Data != ToProcess.end()) {
          if (!DXILContHelper::isRematerializableLgcRtOp(CInst,
                                                         Data->second.Kind))
            Data->second.IntrinsicCalls.push_back(&CInst);
        }
      });
    }
  }
}

void LowerRaytracingPipelinePassImpl::collectDriverFunctions() {
  IsEndSearch = GpurtLibrary->getFunction("_cont_IsEndSearch");
  GetTriangleHitAttributes =
      GpurtLibrary->getFunction("_cont_GetTriangleHitAttributes");
  SetTriangleHitAttributes =
      GpurtLibrary->getFunction("_cont_SetTriangleHitAttributes");
  GetLocalRootIndex = GpurtLibrary->getFunction("_cont_GetLocalRootIndex");
  SetLocalRootIndex = getSetLocalRootIndex(*Mod);
  SetupRayGen = GpurtLibrary->getFunction("_cont_SetupRayGen");
  TraceRay = GpurtLibrary->getFunction("_cont_TraceRay");
  CallShader = GpurtLibrary->getFunction("_cont_CallShader");
  ReportHit = GpurtLibrary->getFunction("_cont_ReportHit");
  AcceptHit = GpurtLibrary->getFunction("_cont_AcceptHit");
}

LowerRaytracingPipelinePassImpl::LowerRaytracingPipelinePassImpl(
    llvm::Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Context{&M.getContext()},
      DL{&M.getDataLayout()}, Builder{Mod->getContext()}, MetadataState{*Mod},
      Mutator{*Mod}, PAQManager{Mod, &GpurtLibrary,
                                MetadataState.getMaxPayloadRegisterCount()} {}

bool LowerRaytracingPipelinePassImpl::run() {
  MetadataState.updateModuleMetadata();

  collectDriverFunctions();

  collectProcessableFunctions();

  struct VisitorState {
    PAQSerializationInfoManager &PAQManager;
    MapVector<Function *, FunctionData> &Processables;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByInstruction)
          .addSet<TraceRayOp, CallCallableShaderOp, ReportHitOp,
                  ShaderIndexOp>([](VisitorState &State, Instruction &Op) {
            auto *CInst = cast<CallInst>(&Op);
            auto Data = State.Processables.find(CInst->getFunction());
            if (Data == State.Processables.end())
              return;

            if (isa<ShaderIndexOp>(Op)) {
              Data->second.ShaderIndexCalls.push_back(CInst);
              return;
            }

            Type *PayloadTy =
                DXILContHelper::getPayloadTypeFromMetadata(*CInst);

            if (!isa<ReportHitOp>(Op)) {
              PAQPayloadConfig PAQPayload = {
                  PayloadTy, Data->second.FuncConfig.MaxHitAttributeBytes};

              uint32_t PayloadStorageI32s = 0;
              if (isa<TraceRayOp>(Op)) {
                PayloadStorageI32s =
                    State.PAQManager.getMaxPayloadStorageI32sForTraceRayFunc(
                        PAQPayload);

                Data->second.TraceRayCalls.push_back(CInst);
              } else if (isa<CallCallableShaderOp>(Op)) {
                PayloadStorageI32s =
                    State.PAQManager.getMaxPayloadStorageI32sForCallShaderFunc(
                        PAQPayload);

                Data->second.CallShaderCalls.push_back(CInst);
              }

              Data->second.MaxOutgoingPayloadI32s = std::max(
                  Data->second.MaxOutgoingPayloadI32s, PayloadStorageI32s);
            } else {
              // The converter uses payload type metadata also to indicate hit
              // attribute types
              assert((!Data->second.HitAttributes ||
                      Data->second.HitAttributes == PayloadTy) &&
                     "Multiple reportHit calls with different hit attributes");
              Data->second.HitAttributes = PayloadTy;

              Data->second.ReportHitCalls.push_back(CInst);
            }
          })
          .build();

  VisitorState S{PAQManager, ToProcess};
  Visitor.visit(S, *Mod);

  handleUnrematerializableCandidates();
  handleDriverFuncAssertions();

  // Find the traversal system data type by looking at the argument to
  // ReportHit.
  TraversalDataTy = nullptr;
  if (ReportHit)
    TraversalDataTy = getFuncArgPtrElementType(ReportHit, 0);
  HitMissDataTy = nullptr;
  if (auto *HitKind = GpurtLibrary->getFunction("_cont_HitKind")) {
    HitMissDataTy = getFuncArgPtrElementType(HitKind, 0);
    LLVM_DEBUG(dbgs() << "HitMiss system data from _cont_HitKind: ";
               HitMissDataTy->dump());
  }

  createPayloadGlobal();
  setTraversalRegisterCountMetadata();

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
      if (Name.startswith("_cont_TraceRay") ||
          Name.startswith("_cont_CallShader") ||
          Name.startswith("_cont_ReportHit")) {
        F.eraseFromParent();
      }
    }
  }

  fixupDxilMetadata(*Mod);

  for (Function &F : *Mod) {
    // Remove the DXIL Payload Type metadata
    F.setMetadata(DXILContHelper::MDDXILPayloadTyName, nullptr);
  }

  llvm::removeUnusedFunctionDecls(Mod);

  return true;
}

std::optional<PAQShaderStage>
llvm::dxilShaderKindToPAQShaderStage(DXILShaderKind ShaderKind) {
  switch (ShaderKind) {
  case DXILShaderKind::RayGeneration:
    return PAQShaderStage::Caller;
  case DXILShaderKind::Intersection:
    // Explicit: PAQ do not apply to Intersection
    return {};
  case DXILShaderKind::AnyHit:
    return PAQShaderStage::AnyHit;
  case DXILShaderKind::ClosestHit:
    return PAQShaderStage::ClosestHit;
  case DXILShaderKind::Miss:
    return PAQShaderStage::Miss;
  case DXILShaderKind::Callable:
    // Explicit: PAQ do not apply to Callable
    return {};
  default:
    return {};
  }
} // anonymous namespace

llvm::PreservedAnalyses
LowerRaytracingPipelinePass::run(llvm::Module &M,
                                 llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass lower-raytracing-pipeline\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  LowerRaytracingPipelinePassImpl Impl(M, GpurtLibrary ? *GpurtLibrary : M);
  bool Changed = Impl.run();

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
