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
// a new alloca is added, and the rematerializable intrinsics get the new alloca
// as their argument. All these intrinsics cannot modify system data, otherwise
// we could not rematerialize them.
//
// At the start of a function, the alloca is initialized from an argument.

#pragma once

#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/PayloadAccessQualifiers.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Coroutines/CoroSplit.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace llvm_dialects {
class Builder;
class DialectContext;
} // namespace llvm_dialects

namespace llvm {

class PassBuilder;
class PassManagerBuilder;
class SmallBitVector;
struct CoroSplitPass;

// Returns the PAQShaderStage corresponding to the given RayTracingShaderStage,
// if there is any.
std::optional<PAQShaderStage> rtShaderStageToPAQShaderStage(lgc::rt::RayTracingShaderStage ShaderKind);

/// Remove bitcasts of function pointers in metadata.
/// This also removes the DXIL payload metadata from functions.
/// Returns true if something changed.
bool fixupDxilMetadata(Module &M);

/// Get intrinsic to set the local root signature index.
Function *getSetLocalRootIndex(Module &M);

/// Get intrinsic to convert a dx handle to an acceleration struct address.
Function *getAccelStructAddr(Module &M, Type *HandleTy);

/// Get the await intrinsic.
Function *getContinuationAwait(Module &M, Type *TokenTy, StructType *RetTy);

/// Get function that returns the global memory base address if the continuation
/// stack lives in global memory.
Function *getContinuationStackGlobalMemBase(Module &M);

/// Checks if a value is a given global or a cast version of it.
bool isCastGlobal(GlobalValue *Global, Value *V);

uint64_t getInlineHitAttrsBytes(Module &M);

/// Extract a function from a constant metadata node, ignoring any bitcasts.
Function *extractFunctionOrNull(Metadata *N);

/// Based on the metadata of a function, check if this is a start function of a shader.
bool isStartFunc(Function *Func);

/// Recurse into the first member of the given SystemData to find an object of
/// the wanted type.
/// See also the system data documentation at the top of Continuations.h.
Value *getDXILSystemData(IRBuilder<> &B, Value *SystemData, Type *SystemDataTy, Type *Ty);

/// Replace call to intrinsic (lgc.rt.*) with a call to the driver
/// implementation (_cont_*).
CallInst *replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy, Value *SystemData,
                               lgc::rt::RayTracingShaderStage Kind, CallInst *Call, Module *GpurtLibrary,
                               CompilerUtils::CrossModuleInliner &Inliner);

/// Terminate a shader by inserting a return instruction and taking care of
/// basic block splitting and preventing early returns.
void terminateShader(IRBuilder<> &Builder, CallInst *CompleteCall);

/// Transformations that run early on the driver/gpurt module.
///
/// Replace intrinsics called by gpurt code that can be replaced early.
/// Returns whether something changed.
bool earlyDriverTransform(Module &M);

/// Given a number NumI32s of 4-byte values and the number of reserved
/// registers, return the amount of dynamic storage required to store that many
/// 4-byte values, in bytes. Returns 0 if the reserved registers suffice.
uint64_t computePayloadSpillSize(uint64_t NumI32s, uint64_t NumReservedRegisters);

// Given two I32 pointers, copy NumBytes many bytes from Src to Dst.
// The implementation performs I32 copies, plus a copy
// of individual bytes at the end if NumBytes is not a multiple of 4.
void copyBytes(IRBuilder<> &B, Value *Dst, Value *Src, uint64_t NumBytes);

class DialectContextAnalysisResult {
public:
  DialectContextAnalysisResult() {}

  bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &, llvm::ModuleAnalysisManager::Invalidator &) {
    return false;
  }
};

/// An analysis to run with dialects, even if the running tool does not have
/// explicit support for it. This will create a dialect context on-demand.
class DialectContextAnalysis : public llvm::AnalysisInfoMixin<DialectContextAnalysis> {
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

class LegacyCleanupContinuationsPass : public llvm::PassInfoMixin<LegacyCleanupContinuationsPass> {
public:
  LegacyCleanupContinuationsPass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "legacy continuation cleanup"; }
};

class CleanupContinuationsPass : public llvm::PassInfoMixin<CleanupContinuationsPass> {
public:
  CleanupContinuationsPass(bool Use64BitContinuationReferences = false)
      : Use64BitContinuationReferences{Use64BitContinuationReferences} {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

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
  Value *getContinuationFramePtr(Function *F, bool IsStart, const ContinuationData &ContinuationInfo,
                                 SmallVector<Instruction *> *InstsToRemove = nullptr);
  void freeCpsStack(Function *F, ContinuationData &CpsInfo);
  void updateCpsStack(Function *F, Function *NewFunc, bool IsStart, ContinuationData &CpsInfo);
  void analyzeContinuation(Function &F, MDNode *MD);
  void processContinuations();
  void handleContinue(ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(ContinuationData &Data, CallInst *Call, Value *ResumeFun);
  void lowerIntrinsicCall(Module &Mod);
  void lowerGetResumePoint(Module &Mod);

  llvm_dialects::Builder *Builder = nullptr;
  Function *ContMalloc = nullptr;
  Function *ContFree = nullptr;
  MapVector<Function *, ContinuationData> ToProcess;
  uint32_t MaxContStateBytes;
  llvm::Module *GpurtLibrary = nullptr;
  bool Use64BitContinuationReferences;
  llvm::Type *ContinuationReferenceType = nullptr;
};

// Define a wrapper pass that is used for CleanupContinuationsPass creating
// 64-bit lgc.cps.as.continuation.reference ops.
class DXILCleanupContinuationsPass : public CleanupContinuationsPass {
public:
  DXILCleanupContinuationsPass() : CleanupContinuationsPass(true) {}

  static llvm::StringRef name() { return "DXIL cleanup continuations pass wrapper"; }
};

// A pass that reports statistics from the continuations module.
class ContinuationsStatsReportPass : public llvm::PassInfoMixin<ContinuationsStatsReportPass> {
public:
  ContinuationsStatsReportPass() = default;
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Continuations statistics reporting pass"; }
};

class LowerRaytracingPipelinePass : public llvm::PassInfoMixin<LowerRaytracingPipelinePass> {
public:
  LowerRaytracingPipelinePass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Lower raytracing pipeline pass"; }
};

class LgcCpsJumpInlinerPass : public llvm::PassInfoMixin<LgcCpsJumpInlinerPass> {
public:
  LgcCpsJumpInlinerPass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "lgc.cps jump inliner pass"; }
};

class DXILContIntrinsicPreparePass : public llvm::PassInfoMixin<DXILContIntrinsicPreparePass> {
public:
  DXILContIntrinsicPreparePass();
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "DXIL continuation intrinsic preparation"; }
};

class DXILContPostProcessPass : public llvm::PassInfoMixin<DXILContPostProcessPass> {
public:
  DXILContPostProcessPass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "DXIL continuation post processing"; }
};

class ContinuationsLintPass : public llvm::PassInfoMixin<ContinuationsLintPass> {
public:
  ContinuationsLintPass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Continuations lint pass"; }
};

class LowerAwaitPass : public llvm::PassInfoMixin<LowerAwaitPass> {
public:
  LowerAwaitPass();
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "continuation point lowering"; }
};

// No-op pass running before the DXIL continuations pipeline, e.g. for usage
// with -print-after
class DXILContPreHookPass : public llvm::PassInfoMixin<DXILContPreHookPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager) {
    return PreservedAnalyses::all();
  }
  static llvm::StringRef name() { return "DXIL continuation pre hook pass"; }
};

// No-op pass running after the DXIL continuations pipeline, e.g. for usage with
// -print-after
class DXILContPostHookPass : public llvm::PassInfoMixin<DXILContPostHookPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager) {
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
  DXILCoroSplitPass() : CoroSplitPass(std::function<bool(Instruction &)>(&DXILMaterializable), true) {}

  static llvm::StringRef name() { return "DXIL continuations coro split pass wrapper"; }
};

// Helper function to query whether an instruction is rematerializable, which is
// shared between both DX and Vulkan path.
bool commonMaterializable(Instruction &I);

// Rematerializable callback specific to LgcCps - mainly used to extend what's
// considered rematerializable for continuations
bool LgcMaterializable(Instruction &I);

// Define a wrapper pass that is used for testing using opt (lgc-coro-split vs
// coro-split)
class LgcCoroSplitPass : public CoroSplitPass {
public:
  LgcCoroSplitPass() : CoroSplitPass(std::function<bool(Instruction &)>(&LgcMaterializable), true) {}

  static llvm::StringRef name() { return "Lgc continuations coro split pass wrapper"; }
};

// Pass to remove !pointeetys metadata from function definitions and declarations
class RemoveTypesMetadataPass : public llvm::PassInfoMixin<RemoveTypesMetadataPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Remove types metadata"; }
};

class DXILContLgcRtOpConverterPass : public llvm::PassInfoMixin<DXILContLgcRtOpConverterPass> {
public:
  DXILContLgcRtOpConverterPass() = default;
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "Convert DXIL ops into lgc.rt ops"; }

private:
  std::unique_ptr<llvm_dialects::Builder> Builder;
  Module *M = nullptr;
  const llvm::DataLayout *DL = nullptr;

  bool convertDxOp(llvm::Function &Func);
  using OpCallbackType = std::function<llvm::Value *(llvm::CallInst &, DXILContLgcRtOpConverterPass *)>;
  std::optional<OpCallbackType> getCallbackByOpName(StringRef OpName);

  template <typename T> Value *handleSimpleCall(CallInst &CI);
  Value *handleTraceRayOp(CallInst &CI);
  Value *handleReportHitOp(CallInst &CI);
  Value *handleCallShaderOp(CallInst &CI);
  template <typename T, unsigned MaxElements = 3> Value *handleVecResult(CallInst &CI);
  template <typename Op, unsigned MaxRows = 3, unsigned MaxColumns = 4> Value *handleMatrixResult(CallInst &CI);
  Value *createVec3(Value *X, Value *Y, Value *Z);
  void addDXILPayloadTypeToCall(Function &DXILFunc, CallInst &CI);
  bool prepareEntryPointShaders();
  void setupLocalRootIndex(Function *F);
};

/// Add necessary continuation transform passes for LGC.
void addLgcContinuationTransform(ModulePassManager &MPM);

} // namespace llvm
