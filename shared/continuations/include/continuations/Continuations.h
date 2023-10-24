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

//===- Continuations.h - Continuations header -----------------------------===//
//
// This file declares all the continuations passes and helper classes and
// functions.
//
//===----------------------------------------------------------------------===//

// System Data
//
// The following describes how the system data struct is handled across passes.
// The system data are a struct that contains the state maintained by the
// driver. The most basic state are the dispatch id and dimension. While the
// traversal loop is running, a larger struct, containing the basic struct, is
// built, which also contains the traversal state. The DXIL intrinsics
// implemented in gpurt get the system data as a pointer argument.
//
// When calling a function like Traversal or another shader, the system data are
// passed by value and also returned by value. The type of the passed and
// returned struct may be different, e.g. Traversal receives a large struct but
// returns only the basic dispatch data.
//
// There are two classes of DXIL intrinsics that access system data. The ones
// that can be rematerialized, because they only read constant data (this is
// e.g. the dispatch id), and the ones that read changing data or data that is
// not available in the returned, most basic version of the system data and can
// therefore not be rematerialized.
//
// To support non-rematerializable intrinsics like RayTCurrent, the
// LowerRaytracingPipeline pass creates an `alloca` for the system data and all
// of these intrinsics and calls access the alloca. Parts of the alloca can end
// up in the continuation state, e.g. if an old `t` is needed after a resume
// point. A called function may overwrite `t` in the system data or return a
// smaller struct that does not contain `t`, but if `t` is used after a resume
// point, it needs to be saved in the continuation state. We rely on the SROA
// pass to remove the alloca in other cases.
//
// Rematerializable intrinsics like DispatchRaysIndex are left in their lgc.rt
// form and don't access system data until the DXILContPostProcess pass. There,
// a new alloca is added, SetupRayGen is called to create the initial system
// data and the rematerializable intrinsics get the new alloca as their
// argument. All these intrinsics cannot modify system data, otherwise we could
// not rematerialize them.
//
// At the start of a function, the alloca is initialized from
// getSystemData, which is itself initialized from either an argument or
// SetupRayGen.

#ifndef CONTINUATIONS_CONTINUATIONS_H
#define CONTINUATIONS_CONTINUATIONS_H

#include "continuations/ContinuationsUtil.h"
#include "continuations/PayloadAccessQualifiers.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Coroutines/CoroSplit.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace llvm_dialects {
class Builder;
class DialectContext;
} // namespace llvm_dialects

namespace continuations {
class GetSystemDataOp;
} // namespace continuations

namespace llvm {

class PassBuilder;
class PassManagerBuilder;
struct CoroSplitPass;

// Returns the PAQShaderStage corresponding to the given DXILShaderKind, if
// there is any.
std::optional<PAQShaderStage>
dxilShaderKindToPAQShaderStage(DXILShaderKind ShaderKind);

/// Changes the continuation stack pointer by I and returns the old and new CSP
/// value.
std::pair<LoadInst *, Value *> moveContinuationStackOffset(IRBuilder<> &B,
                                                           int32_t I);

/// Convert an offset to the continuation stack to a pointer into the memory
/// where the continuation stack lives.
Value *continuationStackOffsetToPtr(IRBuilder<> &B, Value *Offset);

/// Create a new function based on another function, copying attributes and
/// other properties.
Function *cloneFunctionHeader(Function &F, FunctionType *NewType,
                              ArrayRef<AttributeSet> ArgAttrs);

/// Create a new function, as cloneFunctionHeader, but include types metadata.
Function *cloneFunctionHeaderWithTypes(Function &F, DXILContFuncTy &NewType,
                                       ArrayRef<AttributeSet> ArgAttrs);

/// Remove bitcasts of function pointers in metadata.
/// Returns true if something changed.
bool fixupDxilMetadata(Module &M);

/// Get intrinsic that forms a barrier with some arguments.
/// This is used to connect storing the memory pointer of a register buffer to
/// accessing the buffer and prevent reordering.
Function *getRegisterBufferSetPointerBarrier(Module &M);

/// Create the metadata for a register buffer global.
MDTuple *createRegisterBufferMetadata(LLVMContext &Context,
                                      const RegisterBufferMD &MD);

/// Extract the metadata for a register buffer global.
RegisterBufferMD getRegisterBufferMetadata(const MDNode *MD);

/// Get intrinsic to set the local root signature index.
Function *getSetLocalRootIndex(Module &M);

/// Get intrinsic to convert a dx handle to an acceleration struct address.
Function *getAccelStructAddr(Module &M, Type *HandleTy);

/// Get intrinsic to save the continuation state.
Function *getContinuationSaveContinuationState(Module &M);
/// Get intrinsic to restore the continuation state.
Function *getContinuationRestoreContinuationState(Module &M);
/// Get the continuation.continue intrinsic.
Function *getContinuationContinue(Module &M);
/// Get the continuation.waitContinue intrinsic.
Function *getContinuationWaitContinue(Module &M);
/// Get the continuation.complete intrinsic.
Function *getContinuationComplete(Module &M);
/// Get the await intrinsic.
Function *getContinuationAwait(Module &M, Type *TokenTy, StructType *RetTy);
/// Get the CSP init intrinsic.
Function *getContinuationCspInit(Module &M);

/// Get the type of the continuation stack pointer.
Type *getContinuationStackOffsetType(LLVMContext &Context);

/// Get intrinsic to get the continuation stack offset.
/// This intrinsic will be converted to an alloca, but we need to access the
/// value through multiple passes and it's difficult to re-find an alloca, so we
/// delay creating the actual alloca to a late pass.
Function *getContinuationStackOffset(Module &M);

/// Get function that returns the global memory base address if the continuation
/// stack lives in global memory.
Function *getContinuationStackGlobalMemBase(Module &M);

/// Checks if a value is a given global or a cast version of it.
bool isCastGlobal(GlobalValue *Global, Value *V);

uint64_t getInlineHitAttrsBytes(Module &M);

/// Extract a function from a constant metadata node, ignoring any bitcasts.
Function *extractFunctionOrNull(Metadata *N);

/// Returns true if a call to the given function should be rematerialized
/// in a shader of the specified kind.
///
/// If no shader kind is specified,
bool isRematerializableLgcRtOp(
    CallInst &CInst, std::optional<DXILShaderKind> Kind = std::nullopt);

/// Recurse into the first member of the given SystemData to find an object of
/// the wanted type.
/// See also the system data documentation at the top of Continuations.h.
Value *getDXILSystemData(IRBuilder<> &B, Value *SystemData, Type *SystemDataTy,
                         Type *Ty);

/// Replace call to intrinsic (lgc.rt.*) with a call to the driver
/// implementation (_cont_*).
CallInst *replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy,
                               Value *SystemData, DXILShaderKind Kind,
                               CallInst *Call);

/// Buffered pointers use a fixed number of registers, and fall back to an
/// allocation if the registers to not suffice to contain the content. Given a
/// number NumI32s of 4-byte values and the number of reserved registers, return
/// the amount of dynamic storage required to store that many 4-byte values, in
/// bytes. Returns 0 if the reserved registers suffice.
uint64_t computeNeededStackSizeForRegisterBuffer(uint64_t NumI32s,
                                                 uint64_t NumReservedRegisters);

// Given two I32 pointers, copy NumBytes many bytes from Src to Dst.
// The implementation performs I32 copies, plus a copy
// of individual bytes at the end if NumBytes is not a multiple of 4.
void copyBytes(IRBuilder<> &B, Value *Dst, Value *Src, uint64_t NumBytes);

/// Return element type of a function argument resolving opaque pointers
/// via !types metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Function *F, const Argument *Arg);

/// Return element type of a function argument resolving opaque pointers
/// via !types metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Function *F, int ArgNo);

class DialectContextAnalysisResult {
public:
  DialectContextAnalysisResult() {}

  bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                  llvm::ModuleAnalysisManager::Invalidator &) {
    return false;
  }
};

/// An analysis to run with dialects, even if the running tool does not have
/// explicit support for it. This will create a dialect context on-demand.
class DialectContextAnalysis
    : public llvm::AnalysisInfoMixin<DialectContextAnalysis> {
public:
  using Result = DialectContextAnalysisResult;
  DialectContextAnalysis(bool NeedDialectContext = true);
  Result run(llvm::Module &module, llvm::ModuleAnalysisManager &);
  static llvm::AnalysisKey Key;

private:
  std::unique_ptr<llvm_dialects::DialectContext> Context;
  // If true, this analysis is responsible to create a dialect context.
  // If false, a context is already created outside of the pass pipeline.
  bool NeedDialectContext;
};

class LegacyCleanupContinuationsPass
    : public llvm::PassInfoMixin<LegacyCleanupContinuationsPass> {
public:
  LegacyCleanupContinuationsPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "legacy continuation cleanup"; }

private:
  struct ContinuationData {
    /// All functions belonging to this continuation, the entry function is the
    /// first one
    SmallVector<Function *> Functions;
    /// Size of the continuation state in byte
    uint32_t ContStateBytes = 0;
    CallInst *MallocCall = nullptr;
    MDNode *MD = nullptr;
    AllocaInst *NewContState = nullptr;
    SmallVector<Function *> NewFunctions;
    SmallVector<CallInst *> NewReturnContinues;
    /// Cleaned entry function, used to replace metadata
    Function *NewStart = nullptr;
  };

  void analyzeContinuation(Function &F, MDNode *MD);
  void processContinuations();
  void handleFunctionEntry(IRBuilder<> &B, ContinuationData &Data, Function *F,
                           bool IsEntry);
  void handleContinue(IRBuilder<> &B, ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(IRBuilder<> &B, ContinuationData &Data,
                            CallInst *Call, Value *ResumeFun);
  void handleReturn(IRBuilder<> &B, ContinuationData &Data, CallInst *ContRet);

  Module *M;
  Type *I32;
  Type *I64;
  Function *ContMalloc;
  Function *ContFree;
  Function *SaveContState;
  Function *RestoreContState;
  Function *RegisterBufferSetPointerBarrier;
  Function *Continue;
  Function *Complete;
  GlobalVariable *ContState;
  MapVector<Function *, ContinuationData> ToProcess;
  uint32_t MaxContStateBytes;
};

class CleanupContinuationsPass
    : public llvm::PassInfoMixin<CleanupContinuationsPass> {
public:
  CleanupContinuationsPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "continuation cleanup"; }

private:
  struct ContinuationData {
    /// All functions belonging to this continuation, the entry function is the
    /// first one
    SmallVector<Function *> Functions;
    /// Size of the continuation state in byte
    uint32_t ContStateBytes = 0;
    CallInst *MallocCall = nullptr;
    MDNode *MD = nullptr;
    SmallVector<Function *> NewFunctions;
  };

  void removeContFreeCall(Function *F, Function *ContFree);
  Value *getContinuationFramePtr(Function *F, bool IsStart,
                                 const ContinuationData &ContinuationInfo,
                                 SmallVector<Instruction *> &InstsToRemove);
  void freeCpsStack(Function *F, ContinuationData &CpsInfo);
  void updateCpsStack(Function *F, Function *NewFunc, bool IsStart,
                      ContinuationData &CpsInfo);
  void analyzeContinuation(Function &F, MDNode *MD);
  void processContinuations();
  void handleContinue(ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(ContinuationData &Data, CallInst *Call,
                            Value *ResumeFun);

  llvm_dialects::Builder *Builder;
  Function *ContMalloc;
  Function *ContFree;
  MapVector<Function *, ContinuationData> ToProcess;
  uint32_t MaxContStateBytes;
};
class LowerRaytracingPipelinePass
    : public llvm::PassInfoMixin<LowerRaytracingPipelinePass> {
public:
  LowerRaytracingPipelinePass() = default;
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Lower raytracing pipeline pass"; }
};

class DXILContIntrinsicPreparePass
    : public llvm::PassInfoMixin<DXILContIntrinsicPreparePass> {
public:
  DXILContIntrinsicPreparePass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() {
    return "DXIL continuation intrinsic preparation";
  }
};

class PreCoroutineLoweringPass
    : public llvm::PassInfoMixin<PreCoroutineLoweringPass> {
public:
  PreCoroutineLoweringPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() {
    return "Continuation pre coroutine preparation";
  }

private:
  bool splitBB();
  bool removeInlinedIntrinsics();
  bool lowerGetShaderKind();
  bool lowerGetCurrentFuncAddr();

  Module *Mod;
};

class DXILContPostProcessPass
    : public llvm::PassInfoMixin<DXILContPostProcessPass> {
public:
  DXILContPostProcessPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "DXIL continuation post processing"; }

  struct FunctionData {
    DXILShaderKind Kind = DXILShaderKind::Invalid;
    /// Calls to hlsl intrinsics
    SmallVector<CallInst *> IntrinsicCalls;
    /// Calls to get the system data pointer
    SmallVector<continuations::GetSystemDataOp *> GetSystemDataCalls;

    /// If this is the start function part of a split function
    bool IsStart = true;
    /// Pointer to the alloca'd system data object in this function
    Value *SystemData = nullptr;
    Type *SystemDataTy = nullptr;
  };

private:
  // Returns whether changes were made
  bool
  lowerGetResumePointAddr(llvm::Module &M, llvm::IRBuilder<> &B,
                          const MapVector<Function *, FunctionData> &ToProcess);
  void handleInitialContinuationStackPtr(IRBuilder<> &B, Function &F);
  void handleLgcRtIntrinsic(Function &F);
  void handleRegisterBufferSetPointerBarrier(Function &F,
                                             GlobalVariable *Payload);
  void handleRegisterBufferGetPointer(IRBuilder<> &B, Function &F,
                                      GlobalVariable *Payload);
  void handleValueI32Count(IRBuilder<> &B, Function &F);
  void handleValueGetI32(IRBuilder<> &B, Function &F);
  void handleValueSetI32(IRBuilder<> &B, Function &F);

  void handleContPayloadRegisterI32Count(Function &F);
  void handleContPayloadRegistersGetI32(IRBuilder<> &B, Function &F);
  void handleContPayloadRegistersSetI32(IRBuilder<> &B, Function &F);
  void handleContStackAlloc(FunctionAnalysisManager &FAM, IRBuilder<> &B,
                            Function &F);

  Module *Mod;
  GlobalVariable *Registers;
  MapVector<Function *, FunctionData> ToProcess;
};

class LowerAwaitPass : public llvm::PassInfoMixin<LowerAwaitPass> {
public:
  LowerAwaitPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "continuation point lowering"; }
};

class RegisterBufferPass : public llvm::PassInfoMixin<RegisterBufferPass> {
public:
  RegisterBufferPass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "register buffer lowering"; }

  /// Handle a load/store that accesses a single register only.
  Value *handleSingleLoadStore(IRBuilder<> &Builder, Type *Ty, Value *StoreVal,
                               Value *Address, Align Alignment,
                               AAMDNodes AATags, bool IsLoad);

private:
  /// Convert Address into an address that accesses the memory base address
  /// instead of the register global.
  Value *computeMemAddr(IRBuilder<> &Builder, Value *Address);

  void handleLoadStore(IRBuilder<> &Builder, Instruction *I, Value *Address,
                       bool IsLoad);

  /// Maps a Value that accesses the register part of the global to a Value that
  /// accesses the memory part.
  DenseMap<Value *, Value *> MemAccessors;

  // Properties of the current item that is worked on
  GlobalVariable *Global;
  IntegerType *ElementType;
  RegisterBufferMD Data;
  uint32_t TotalElementCount;
};

class SaveContinuationStatePass
    : public llvm::PassInfoMixin<SaveContinuationStatePass> {
public:
  SaveContinuationStatePass();
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "save continuation state"; }

private:
  /// Returns true if something changed.
  bool lowerCalls(Function *Intr, bool IsSave);
  bool lowerContStateGetPointer();

  void lowerCsp(Function *GetCsp);

  Type *I32;
  IRBuilder<> *B;
  Module *Mod;
  GlobalVariable *ContState;
};

// No-op pass running before the DXIL continuations pipeline, e.g. for usage
// with -print-after
class DXILContPreHookPass : public llvm::PassInfoMixin<DXILContPreHookPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager) {
    return PreservedAnalyses::all();
  }
  static llvm::StringRef name() { return "DXIL continuation pre hook pass"; }
};

// No-op pass running after the DXIL continuations pipeline, e.g. for usage with
// -print-after
class DXILContPostHookPass : public llvm::PassInfoMixin<DXILContPostHookPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager) {
    return PreservedAnalyses::all();
  }
  static llvm::StringRef name() { return "DXIL continuation post hook pass"; }
};

// Rematerializable callback specific to DXIL - mainly used to extend what's
// considered rematerializable for continuations
bool DXILMaterializable(Instruction &I);

// Define a wrapper pass that is used for testing using opt (dxil-coro-split vs
// coro-split)
class DXILCoroSplitPass : public CoroSplitPass {
public:
  DXILCoroSplitPass()
      : CoroSplitPass(std::function<bool(Instruction &)>(&DXILMaterializable),
                      true) {}

  static llvm::StringRef name() {
    return "DXIL continuations coro split pass wrapper";
  }
};

// Rematerializable callback specific to LgcCps - mainly used to extend what's
// considered rematerializable for continuations
bool LgcMaterializable(Instruction &I);

// Define a wrapper pass that is used for testing using opt (lgc-coro-split vs
// coro-split)
class LgcCoroSplitPass : public CoroSplitPass {
public:
  LgcCoroSplitPass()
      : CoroSplitPass(std::function<bool(Instruction &)>(&LgcMaterializable),
                      true) {}

  static llvm::StringRef name() {
    return "Lgc continuations coro split pass wrapper";
  }
};

// Pass to remove !types metadata from function definitions and declarations
class RemoveTypesMetadataPass
    : public llvm::PassInfoMixin<RemoveTypesMetadataPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Remove types metadata"; }
};

class DXILContLgcRtOpConverterPass
    : public llvm::PassInfoMixin<DXILContLgcRtOpConverterPass> {
public:
  DXILContLgcRtOpConverterPass() = default;
  llvm::PreservedAnalyses run(llvm::Module &Module,
                              llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Convert DXIL ops into lgc.rt ops"; }

private:
  std::unique_ptr<llvm_dialects::Builder> Builder;
  Module *M = nullptr;
  const llvm::DataLayout *DL = nullptr;

  bool processFunction(llvm::Function &Func);
  using OpCallbackType = std::function<llvm::Value *(
      llvm::CallInst &, DXILContLgcRtOpConverterPass *)>;
  std::optional<OpCallbackType> getCallbackByOpName(StringRef OpName);

  template <typename T> Value *handleSimpleCall(CallInst &CI);
  Value *handleTraceRayOp(CallInst &CI);
  Value *handleReportHitOp(CallInst &CI);
  Value *handleCallShaderOp(CallInst &CI);
  template <typename T, unsigned MaxElements = 3>
  Value *handleVecResult(CallInst &CI);
  template <typename Op, unsigned MaxRows = 3, unsigned MaxColumns = 4>
  Value *handleMatrixResult(CallInst &CI);
  Value *createVec3(Value *X, Value *Y, Value *Z);
  void addDXILPayloadTypeToCall(Function &DXILFunc, CallInst &CI);
  void applyPayloadMetadataTypesOnShaders();
};

/// Add necessary continuation transform passes for LGC.
void addLgcContinuationTransform(ModulePassManager &MPM);

/// LLVM parser callback which adds !types metadata during DXIL parsing
void DXILValueTypeMetadataCallback(Value *V, unsigned TypeID,
                                   GetTypeByIDTy GetTypeByID,
                                   GetContainedTypeIDTy GetContainedTypeID);
} // namespace llvm

#endif
