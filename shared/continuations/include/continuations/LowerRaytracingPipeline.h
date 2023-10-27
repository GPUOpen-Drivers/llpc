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

//===- LowerRaytracingPipeline.h ------------------------------------------===//
//
// Declare the class which implements the lowering of lgc.rt operations within
// the coroutine support.
//
//===----------------------------------------------------------------------===//

#ifndef CONTINUATIONS_LOWERRAYTRACINGPIPELINE_H
#define CONTINUATIONS_LOWERRAYTRACINGPIPELINE_H

#include "continuations/ContinuationsUtil.h"
#include "continuations/PayloadAccessQualifiers.h"
#include "lgc/LgcCpsDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include <cstdint>

namespace llvm {
class AllocaInst;
class CallInst;
class Function;
class GlobalVariable;
class Instruction;
class Module;
class StructType;
class Type;
class Value;

namespace {
enum class ContinuationCallType {
  Traversal,
  CallShader,
  AnyHit,
};
} // namespace

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

  ContStackAddrspace getContStackAddrspace() const { return StackAddrspace; };

  bool isGlobalAddressSpace() const {
    return StackAddrspace == ContStackAddrspace::Global;
  }

  [[maybe_unused]] bool isScratchAddressSpace() const {
    return StackAddrspace == ContStackAddrspace::Scratch;
  }

  void updateModuleMetadata() const;

private:
  Module &Mod;
  /// MaxPayloadRegisterCount is initialized from metadata. If there is none,
  /// use this default instead:
  static constexpr uint32_t DefaultPayloadRegisterCount = 30;
  /// Maximum allowed number of registers to be used for the payload.
  uint32_t MaxPayloadRegisterCount = 0;
  //// Minimum required number of payload registers.
  uint32_t MinPayloadRegisterCount = 0;
  /// The address space used for the continuations stack.
  /// Either stack or global memory.
  ContStackAddrspace StackAddrspace = DefaultStackAddrspace;
  static constexpr ContStackAddrspace DefaultStackAddrspace =
      ContStackAddrspace::Scratch;
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
  LowerRaytracingPipelinePassImpl(Module &M);
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
    std::string getFunctionSuffix() const {
      std::string Result;
      raw_string_ostream Stream{Result};
      Stream << ".attr_max_" << MaxHitAttributeBytes << "_bytes";
      return Result;
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

  struct AwaitFunctionData {
    DXILShaderKind CallerKind = DXILShaderKind::Invalid;
    ContinuationCallType CallType;
    SmallVector<CallInst *> AwaitCalls;
    FunctionConfig FuncConfig = {};

    bool operator==(const AwaitFunctionData &Other) const {
      return std::tie(CallType, AwaitCalls, FuncConfig) ==
             std::tie(Other.CallType, Other.AwaitCalls, FuncConfig);
    }
  };

  static DXILShaderKind callTypeToShaderKind(ContinuationCallType CallType);

  CallInst *replaceCall(IRBuilder<> &B, FunctionData &Data, CallInst *Call,
                        Function *Func, StringRef NewName,
                        ContinuationCallType CallType);
  void handleRestoreSystemData(IRBuilder<> &B, CallInst *Call);
  void replaceTraceRayCall(IRBuilder<> &B, FunctionData &Data, CallInst *Call);
  void replaceCallShaderCall(IRBuilder<> &B, FunctionData &Data,
                             CallInst *Call);
  void replaceContinuationCall(IRBuilder<> &B, ContinuationCallType CallType,
                               CallInst *Call, const FunctionConfig &FuncConfig,
                               DXILShaderKind CallerKind);
  void replaceReportHitCall(llvm_dialects::Builder &B, FunctionData &Data,
                            CallInst *Call);

  void handleReportHit(FunctionData &Data, Function &F);

  void replaceShaderIndexCall(IRBuilder<> &B, FunctionData &Data,
                              CallInst *Call);

  void handleContinuationStackIsGlobal(Function &Func);

  void handleGetFuncAddr(Function &Func);

  void handleAmdInternalFunc(Function &Func);

  void handleUnrematerializableCandidates();

  void collectDriverFunctions();

  void handleGetUninitialized(Function &Func);

  // Copy the payload content between global payload and local payload.
  // Excludes the stack pointer or hit attributes which may also reside in
  // payload storage. If Stage is not set, all fields in SerializationInfo are
  // copied. Used for CallShader accesses which are not PAQ qualified and do not
  // have PAQShaderStage values.
  // If CopiedNodes is set, nodes contained will not be copied, and all copied
  // nodes are added to it.
  void copyPayload(IRBuilder<> &B, Type &PayloadTy, Value *LocalPayload,
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
      IRBuilder<> &B, const PAQTraceRaySerializationInfo &PAQSerializationInfo,
      Value *LocalPayload);

  // Caller-save payload registers before CallShader() or TraceRay(),
  // which can override payload registers. A register needs to be saved
  // if it is live in OutgoingLayout, and not written in OutgoingLayout.
  // This includes the payload memory pointer if present.
  // SavedRegisters maps indices of payload registers to their saved values.
  void savePayloadRegistersBeforeRecursion(
      IRBuilder<> &B, DXILShaderKind Kind,
      const PAQSerializationLayout &IncomingLayout,
      const PAQSerializationLayout &OutgoingLayout,
      SmallVectorImpl<Value *> &SavedRegisterValues);

  // Restore previously saved registers.
  void restorePayloadRegistersAfterRecursion(
      IRBuilder<> &B, const SmallVectorImpl<Value *> &SavedRegisterValues);

  void createPayloadGlobal();

  void setTraversalRegisterCountMetadata();

  void copyHitAttributes(IRBuilder<> &B, FunctionData &Data, Value *SystemData,
                         Type *SystemDataTy, Value *LocalHitAttributes,
                         bool GlobalToLocal,
                         const PAQSerializationLayout *Layout);
  void processContinuations();
  void processFunctionEntry(llvm_dialects::Builder &B, Function *F,
                            FunctionData &Data);
  void processFunctionEnd(llvm_dialects::Builder &B, FunctionData &Data,
                          FunctionEndData &EData);
  void processFunction(llvm_dialects::Builder &B, Function *F,
                       FunctionData &FuncData);

  void collectProcessableFunctions();

  void handleDriverFuncAssertions();

  constexpr static uint32_t ArgContState = 0;
  constexpr static uint32_t ArgReturnAddr = 1;
  constexpr static uint32_t ArgShaderIndex = 2;
  constexpr static uint32_t ArgSystemData = 3;
  constexpr static uint32_t ArgHitAttributes = 4;

  MapVector<Function *, FunctionData> ToProcess;
  MapVector<Function *, AwaitFunctionData> AwaitsToProcess;
  Module *Mod;
  LLVMContext *Context;
  const DataLayout *DL;
  ModuleMetadataState MetadataState;
  CpsMutator Mutator;
  PAQSerializationInfoManager PAQManager;
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

  SmallVector<Function *> Awaits;
  SmallVector<Function *> RestoreSystemDatas;
  SmallVector<Value *> EntriesWithPayloadTypeMetadata;

  // We specialize certain intrinsics that lead to suspend-points (TraceRay,
  // CallShader, ReportHit) based on the payload or hit attribute type.
  // We store these types (either payload or hit attribute) here for later use.
  DenseMap<Function *, Type *> PayloadOrAttrTypesForSpecializedFunctions;
};

} // namespace llvm

#endif
