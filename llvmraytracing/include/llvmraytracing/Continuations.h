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

/// Get intrinsic to convert a dx handle to an acceleration struct address.
Function *getAccelStructAddr(Module &M, Type *HandleTy);

/// Get function that returns the global memory base address if the continuation
/// stack lives in global memory.
Function *getContinuationStackGlobalMemBase(Module &M);

/// Checks if a value is a given global or a cast version of it.
bool isCastGlobal(GlobalValue *Global, Value *V);

uint64_t getInlineHitAttrsBytes(Module &M);

/// Extract a function from a constant metadata node, ignoring any bitcasts.
Function *extractFunctionOrNull(Metadata *N);

/// Based on the metadata of a function, get the start function of a continuation shader or resume function.
/// For non-resume functions, returns Func, even if Func is not a continuation shader.
Function *getStartFunc(Function *Func);
/// Returns whether getStartFunc(Func) == Func, see getStartFunc above.
bool isStartFunc(Function *Func);

/// Recurse into the first member of the given SystemData to find an object of
/// the wanted type.
/// See also the system data documentation at the top of Continuations.h.
Value *getDXILSystemData(IRBuilder<> &B, Value *SystemData, Type *SystemDataTy, Type *Ty);

/// Replace call to intrinsic (lgc.rt.*) with a call to the driver
/// implementation (_cont_*).
Value *replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy, Value *SystemData, lgc::rt::RayTracingShaderStage Kind,
                            CallInst *Call, Module *GpurtLibrary, compilerutils::CrossModuleInliner &Inliner,
                            bool KeepBuilderPos = false);

/// Promote pointer arguments of a GPURT function @Func to by-value if appropriate (e. g. depending on pointeetys
/// metadata).
///
/// Changes pointer types to their value types for non-struct types.
/// Handle _Amd*Await* and _Amd*Enqueue*.
/// For _cont_SetTriangleHitAttributes, we always use its value type for hitAttributes argument.
// For Traversal, promote the system data argument so it is of struct type.
///
/// Returns a pointer to the promoted function or nullptr.
Function *tryGpurtPointerArgPromotion(Function *Func);

/// Transformations that run early on the driver/gpurt module.
///
/// Promote arguments of the functions residing in @PromotableFunctions.
/// Replace intrinsics called by gpurt code that can be replaced early.
/// Returns whether something changed.
bool earlyGpurtTransform(Module &M, SmallVector<Function *> &PromotableFunctions, bool PreserveWaitMasks = true);

/// Given a number NumI32s of 4-byte values and the number of reserved
/// registers, return the amount of dynamic storage required to store that many
/// 4-byte values, in bytes. Returns 0 if the reserved registers suffice.
uint64_t computePayloadSpillSize(uint64_t NumI32s, uint64_t NumReservedRegisters);

// Given two I32 pointers, copy NumBytes many bytes from Src to Dst.
// The implementation performs I32 copies, plus a copy
// of individual bytes at the end if NumBytes is not a multiple of 4.
void copyBytes(IRBuilder<> &B, Value *Dst, Value *Src, uint64_t NumBytes);

class CleanupContinuationsPass : public llvm::PassInfoMixin<CleanupContinuationsPass> {
public:
  CleanupContinuationsPass() {}
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);

  static llvm::StringRef name() { return "continuation cleanup"; }
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

class DXILContPrepareGpurtLibraryPass : public llvm::PassInfoMixin<DXILContPrepareGpurtLibraryPass> {
public:
  DXILContPrepareGpurtLibraryPass();
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

// Define a wrapper pass that is used for testing using opt (dxil-coro-split vs
// coro-split)
class DXILCoroSplitPass : public CoroSplitPass {
public:
  DXILCoroSplitPass();

  static llvm::StringRef name() { return "DXIL continuations coro split pass wrapper"; }
};

// Define a wrapper pass that is used for testing using opt (lgc-coro-split vs
// coro-split)
class LgcCoroSplitPass : public CoroSplitPass {
public:
  LgcCoroSplitPass();

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
