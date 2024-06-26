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
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
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

//===- ContinuationsUtil.h - Continuations utility header -----------------===//
//
// This file declares helper classes and functions for continuation passes.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llpc/GpurtEnums.h"
#include "llpc/GpurtVersion.h"
#include "llvm-dialects/Dialect/OpMap.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace llvm {

class Argument;
class Function;
class FunctionType;
class Metadata;
class PassBuilder;
class Type;

/// Size of one register in bytes.
const unsigned RegisterBytes = 4;
/// Address space used for globals that should be put into registers.
const unsigned GlobalRegisterAddrspace = 20;
/// The (first) register used for the memory pointer in payload registers.
/// Currently, it is only a single register for the 32-bit pointer.
const unsigned FirstPayloadMemoryPointerRegister = 0;
/// The first register used for hit attribute storage in payload registers.
/// We need to use a fixed offset: ReportHit (called from intersection shaders)
/// does not know the payload type, but may need to access hit attributes.
const unsigned FirstPayloadHitAttributeStorageRegister = 1;
/// Maximum size of hit attributes in bytes.
/// = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES
/// Smaller limits may be specified in metadata.
const unsigned GlobalMaxHitAttributeBytes = 32;
/// We tell the LLVM coroutine passes the size of a preallocated buffer
/// for the continuation state that can be used without dynamic allocations.
/// If the continuation state is larger, coroutine passes will use a special
/// malloc call that will be replaced later. If we find the malloc, we know
/// the exact continuation state size. If we don't find a malloc, but there
/// are usages of the frame pointer, we need to pessimistically assume
/// that the full size is required.
/// TODO: Figure out whether we can pass a fixed size of 0, eliminating
///       this pessimism.
const unsigned MinimumContinuationStateBytes = 8;

constexpr uint32_t CpsArgIdxContState = 0;
constexpr uint32_t CpsArgIdxReturnAddr = 1;
constexpr uint32_t CpsArgIdxShaderIndex = 2;
constexpr uint32_t CpsArgIdxSystemData = 3;
constexpr uint32_t CpsArgIdxHitAttributes = 4;
constexpr uint32_t CpsArgIdxPadding = 5;
constexpr uint32_t CpsArgIdxPayload = 6;

struct DxRayIntrinsic {
  unsigned int Id;
  StringRef Name;
};

struct GpuRtIntrinsicEntry {
  StringRef Name;
  bool AccessesHitData = false;
};

extern const llvm_dialects::OpMap<GpuRtIntrinsicEntry> LgcRtGpuRtMap;

llvm::raw_ostream &operator<<(llvm::raw_ostream &, DXILShaderKind);

enum class AnyHitExitKind {
  None, // not an AnyHit shader
  IgnoreHit,
  AcceptHit,
  AcceptHitAndEndSearch
};

// The address space used for the continuation stack.
enum class ContStackAddrspace : uint32_t { Scratch = 21, Global = 22 };

// Metadata associated with a register buffer.
struct RegisterBufferMD {
  /// Number of registers to use.
  uint32_t RegisterCount;
  /// Address space for the memory part of the buffer.
  uint32_t Addrspace;
};

// Helper class to abstract over function argument types.
// Derives types from custom metadata when available, allowing pointer
// element types to be derives even with opaque pointers.
class ContArgTy {
private:
  Type *ArgTy;
  Type *ElemTy;

public:
  ContArgTy() : ArgTy(nullptr), ElemTy(nullptr) {}
  ContArgTy(Type *Arg, Type *Elem) : ArgTy(Arg), ElemTy(Elem) {}
  ContArgTy(Type *Arg);

  static ContArgTy get(const Function *F, const Argument *Arg);
  static ContArgTy get(const Function *F, const unsigned ArgNo);
  static ContArgTy get(const Metadata *MD, LLVMContext &Context);

  Type *asType(LLVMContext &Context);
  Type *getPointerElementType() const;

  bool isPointerTy() const;
  bool isVoidTy() const;
  Metadata *getTypeMetadata(LLVMContext &Context);

  bool operator==(const ContArgTy &RHS) const {
    return (ArgTy == RHS.ArgTy) && (ElemTy == RHS.ElemTy);
  }
};

// Helper class to abstract over function types.
// Uses ContArgTy to derive types from and encode types to custom metadata.
class ContFuncTy {
public:
  ContFuncTy() {}
  ContFuncTy(ContArgTy Return) : ReturnTy(Return) {}
  ContFuncTy(ContArgTy Return, ArrayRef<ContArgTy> Args)
      : ReturnTy(Return), ArgTys(Args) {}

  ContArgTy ReturnTy;
  SmallVector<ContArgTy> ArgTys;

  static ContFuncTy get(const Function *F);
  static ContFuncTy get(const Metadata *MD, LLVMContext &Context);

  FunctionType *asFunctionType(LLVMContext &Context);
  void writeMetadata(Function *F);
};

struct ContSetting {
  /// A hash value that is used as name.
  uint64_t NameHash;
  /// Value of the setting
  uint64_t Value;
};

// Helper class to access data specific to continuation passes, e.g.
// metadata or globals.
class ContHelper {
private:
  // Private metadata node names
  // These are private because we provide dedicated utilities to get and set
  // the associated metadata values.

  /////////////////////////////////////////////////////////////////////////////
  // Register count metadata
  //
  // Continuation passes manage a set of registers to pass data between RT
  // stages (see payload registers below), and possibly to store continuation
  // state in. These registers may be referred to "middle-end managed registers"
  // or "payload registers" elsewhere. Note that "payload registers" has a
  // different, more restricted meaning in this context here, see below. In
  // continuation passes, these registers are represented by globals in specific
  // address spaces.
  //
  // The number of registers entering a function (if used as function
  // metadata), or leaving a function (if used on a continue statement).
  static constexpr const char *MDRegisterCountName =
      "continuation.registercount";
  // The number of registers returned by a TraceRay or CallShader call,
  // annotated to the outgoing continue call. For resume functions, we scan
  // continue calls referencing the resume function, and use their returned
  // register count annotation as incoming register count for the resume
  // function.
  static constexpr const char *MDReturnedRegisterCountName =
      "continuation.returnedRegistercount";

  // Module-scope *payload* register count metadata
  // Payload registers are registers used to pass data between RT stages.
  // Most prominently, this may be the app payload, but also storage
  // for intersection hit attributes. Note that the payload is not stored in its
  // bitwise layout, but instead we use "serialization layouts" that account
  // for PAQed fields, and all other data required in a particular stage (e.g.
  // hit attributes).
  //
  // [in] PreservedPayloadRegisterCount:
  // The required number of preserved payload registers for functions that
  // are not aware of payload types (e.g. Intersection or Traversal), if known.
  // This gives an upper bound on the number of payload registers used by other
  // functions together with functions in the current module.
  // Setting this value can be used to reduce the number of preserved registers
  // for such functions to prevent having to preserve the maximum possible
  // amount of payload registers. This is used when compiling a specialized
  // Traversal function for a pipeline after all shaders in the pipeline have
  // been processed.
  // For intersection, it is not used, because early-compiled intersection
  // shaders can be used in pipelines with large payload types unknown when
  // compiling the intersection shader.
  static constexpr const char *MDPreservedPayloadRegisterCountName =
      "continuation.preservedPayloadRegisterCount";
  // [in] MaxPayloadRegisterCount
  // The maximum allowed number of payload registers to be used for payload and
  // other inter-stage date (e.g. attributes). If state does not fit into this
  // limit, we spill to the continuation stack.
  static constexpr const char *MDMaxPayloadRegisterCountName =
      "continuation.maxPayloadRegisterCount";
  // [out] MaxUsedPayloadRegisterCount
  // The maximum number of payload registers written or read by any
  // shader in the module. This excludes intersection shaders, which
  // just pass through an existing payload.
  // This can be used to populate PreservedPayloadRegisterCount when compiling
  // the driver module in case all modules of the pipeline are known and
  // have already been processed.
  static constexpr const char *MDMaxUsedPayloadRegisterCountName =
      "continuation.maxUsedPayloadRegisterCount";
  // The address space used to store the continuations stack.
  // The possible values for this metadata are the values of ContStackAddrspace.
  static constexpr const char *MDStackAddrspaceName =
      "continuation.stackAddrspace";
  // The raytracing ip level that is available on the target architecture.
  // This is exposed to gpurt code via the GetRtip intrinsic.
  static constexpr const char *MDRtipName = "continuation.rtip";
  // Flags set for continuations.
  // This is exposed to gpurt code via the ContinuationsGetFlags intrinsic.
  static constexpr const char *MDFlagsName = "continuation.flags";
  // Marks an await as a waiting one with a wait mask.
  static constexpr const char *MDIsWaitAwaitName = "continuation.wait.await";

  static std::optional<uint32_t> extractZExtI32Constant(MDNode *Node) {
    if (Node) {
      uint64_t Result =
          mdconst::extract<ConstantInt>(Node->getOperand(0))->getZExtValue();
      assert(Result <= std::numeric_limits<uint32_t>::max());
      return Result;
    }
    return {};
  }

  static MDNode *getI32MDConstant(LLVMContext &Context, uint32_t Value) {
    IntegerType *Int32Ty = Type::getInt32Ty(Context);
    MDNode *Result = MDTuple::get(
        Context, {ConstantAsMetadata::get(ConstantInt::get(Int32Ty, Value))});
    assert(Result && "Failed to create metadata node!");
    assert(extractZExtI32Constant(Result) == Value &&
           "Failed to extract value from node!");
    return Result;
  }

  static Type *getPayloadTypeFromMetadata(const MDNode *Node) {
    auto *MDTup = cast<MDTuple>(Node);
    if (auto *ExtractedConstant =
            mdconst::extract<Constant>(MDTup->getOperand(0))) {
      return ExtractedConstant->getType();
    }

    report_fatal_error("Not able to determine Payload type!");
  }

public:
  // Public metadata node names
  static constexpr const char *MDEntryName = "continuation.entry";
  static constexpr const char *MDStackSizeName = "continuation.stacksize";
  static constexpr const char *MDStateName = "continuation.state";
  static constexpr const char *MDContinuationName = "continuation";
  static constexpr const char *MDTypesName = "types";
  static constexpr const char *MDTypesFunctionName = "function";
  static constexpr const char *MDTypesVoidName = "void";
  static constexpr const char *MDContPayloadTyName = "cont.payload.type";
  static constexpr const char *MDLgcCpsModuleName = "lgc.cps.module";
  static constexpr const char *MDGpurtSettingsName = "gpurt.settings";

  // Global variable names
  static constexpr const char *GlobalPayloadName = "PAYLOAD";
  static constexpr const char *GlobalRegistersName = "REGISTERS";
  static constexpr ContStackAddrspace DefaultStackAddrspace =
      ContStackAddrspace::Scratch;

  static void RegisterPasses(llvm::PassBuilder &PB, bool NeedDialectContext);

  // Registers the generic Continuation pipeline to a LLVM Module Pass manager.
  static void addContinuationPasses(llvm::ModulePassManager &MPM);

  // Registers the DXIL-specific Continuation pipeline to a LLVM Module Pass
  // manager.
  static void addDxilContinuationPasses(llvm::ModulePassManager &MPM,
                                        llvm::Module *GpurtLibrary = nullptr);

  // Registers the DXIL-specific pipeline for the driver library module to a
  // LLVM Module Pass manager. These passes preprocess the driver library into a
  // form that can be used for the later continuation passes that are run on app
  // modules.
  static void addDxilGpurtLibraryPasses(llvm::ModulePassManager &MPM);

  // Get gpurt settings from metadata.
  static void getGpurtSettings(const Module &M,
                               SmallVectorImpl<ContSetting> &Settings) {
    auto *MD = M.getNamedMetadata(MDGpurtSettingsName);
    if (!MD)
      return;
    auto *Tup = MD->getOperand(0);

    // Stored as {name, value, name, value, ...}
    for (auto *Op = Tup->op_begin(); Op != Tup->op_end(); ++Op) {
      ContSetting Setting;
      Setting.NameHash = mdconst::extract<ConstantInt>(*Op)->getZExtValue();
      ++Op;
      Setting.Value = mdconst::extract<ConstantInt>(*Op)->getZExtValue();
      Settings.push_back(Setting);
    }
  };

  // Store gpurt settings in metadata.
  static void setGpurtSettings(Module &M, ArrayRef<ContSetting> Settings) {
    auto *MD = M.getOrInsertNamedMetadata(MDGpurtSettingsName);
    MD->clearOperands();
    auto &Context = M.getContext();
    SmallVector<Metadata *> Vals;
    IntegerType *Int64Ty = Type::getInt64Ty(Context);
    // Stored as {bitwidth, value, bitwidth, value, ...}
    for (auto &Setting : Settings) {
      Vals.push_back(
          ConstantAsMetadata::get(ConstantInt::get(Int64Ty, Setting.NameHash)));
      Vals.push_back(
          ConstantAsMetadata::get(ConstantInt::get(Int64Ty, Setting.Value)));
    }
    MD->addOperand(MDTuple::get(Context, Vals));
  }

  // Set metadata specifying the number of outgoing payload registers.
  static void setOutgoingRegisterCount(Instruction *I, uint32_t RegisterCount) {
    I->setMetadata(MDRegisterCountName,
                   getI32MDConstant(I->getContext(), RegisterCount));
  }

  // Get the number of outgoing payload registers if set.
  static std::optional<uint32_t>
  tryGetOutgoingRegisterCount(const Instruction *I) {
    return extractZExtI32Constant(I->getMetadata(MDRegisterCountName));
  }

  // Set metadata specifying the number of incoming payload registers.
  static void setIncomingRegisterCount(Function *F, uint32_t RegisterCount) {
    F->setMetadata(MDRegisterCountName,
                   getI32MDConstant(F->getContext(), RegisterCount));
  }

  // Get the number of incoming payload registers if set.
  static std::optional<uint32_t>
  tryGetIncomingRegisterCount(const Function *F) {
    return extractZExtI32Constant(F->getMetadata(MDRegisterCountName));
  }

  // Set metadata specifying the number of payload registers returned by a
  // TraceRay or CallShader. See MDReturnedRegisterCountName for details.
  static void setReturnedRegisterCount(Instruction *I, uint32_t RegisterCount) {
    I->setMetadata(MDReturnedRegisterCountName,
                   getI32MDConstant(I->getContext(), RegisterCount));
  }

  // Get the number of payload registers returned by a TraceRay or CallShader
  // from metadata if set. See MDReturnedRegisterCountName for details.
  static std::optional<uint32_t>
  tryGetReturnedRegisterCount(const Instruction *I) {
    return extractZExtI32Constant(I->getMetadata(MDReturnedRegisterCountName));
  }

  // If there is module-level metadata node, return its value. Otherwise, return
  // std::nullopt.
  static std::optional<uint32_t>
  tryGetPreservedPayloadRegisterCount(const Module &M) {
    auto *MD = M.getNamedMetadata(MDPreservedPayloadRegisterCountName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void
  setPreservedPayloadRegisterCount(Module &M,
                                   uint32_t PreservedPayloadRegisterCount) {
    auto *MD = M.getOrInsertNamedMetadata(MDPreservedPayloadRegisterCountName);
    assert(MD && "Failed to create metadata node!");
    MD->clearOperands();
    MD->addOperand(
        getI32MDConstant(M.getContext(), PreservedPayloadRegisterCount));
  }

  // Old alias until clients are migrated to setPreservedPayloadRegisterCount:
  static void
  setMinPayloadRegisterCount(Module &M,
                             uint32_t PreservedPayloadRegisterCount) {
    setPreservedPayloadRegisterCount(M, PreservedPayloadRegisterCount);
  }

  // If there is module-level metadata specifying the maximum number
  // of payload registers, return that value. Otherwise, return std::nullopt.
  static std::optional<uint32_t>
  tryGetMaxUsedPayloadRegisterCount(const Module &M) {
    auto *MD = M.getNamedMetadata(MDMaxUsedPayloadRegisterCountName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void
  setMaxUsedPayloadRegisterCount(Module &M,
                                 uint32_t MaxUsedPayloadRegisterCount) {
    auto *MD = M.getOrInsertNamedMetadata(MDMaxUsedPayloadRegisterCountName);
    assert(MD && "Failed to create metadata node!");
    MD->clearOperands();
    MD->addOperand(
        getI32MDConstant(M.getContext(), MaxUsedPayloadRegisterCount));
  }

  static std::optional<uint32_t>
  tryGetMaxPayloadRegisterCount(const Module &M) {
    auto *MD = M.getNamedMetadata(MDMaxPayloadRegisterCountName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void setMaxPayloadRegisterCount(Module &M,
                                         uint32_t MaxPayloadRegisterCount) {
    auto *MD = M.getOrInsertNamedMetadata(MDMaxPayloadRegisterCountName);
    assert(MD && "Failed to create metadata node!");
    MD->clearOperands();
    MD->addOperand(getI32MDConstant(M.getContext(), MaxPayloadRegisterCount));
  }

  static void setStackSize(Function *F, uint32_t StackSize) {
    F->setMetadata(MDStackSizeName,
                   getI32MDConstant(F->getContext(), StackSize));
  }

  // If the function already has stacksize metadata, add the given value.
  // Otherwise, assume an existing value of zero, and set the pass value.
  static void addStackSize(Function *F, uint32_t AddedStackSize) {
    auto ExistingSize = tryGetStackSize(F).value_or(0);
    F->setMetadata(
        MDStackSizeName,
        getI32MDConstant(F->getContext(), ExistingSize + AddedStackSize));
  }

  static std::optional<uint32_t> tryGetStackSize(const Function *F) {
    return extractZExtI32Constant(F->getMetadata(MDStackSizeName));
  }

  // If there is module-level metadata specifying the stack addrspace,
  // return that value. Otherwise, return std::nullopt.
  static std::optional<ContStackAddrspace>
  tryGetStackAddrspace(const Module &M) {
    auto *MD = M.getNamedMetadata(MDStackAddrspaceName);
    if (!MD)
      return {};
    auto AddrSpace = extractZExtI32Constant(MD->getOperand(0));
    if (!AddrSpace)
      return {};
    assert((*AddrSpace == static_cast<uint32_t>(ContStackAddrspace::Scratch) ||
            *AddrSpace == static_cast<uint32_t>(ContStackAddrspace::Global)) &&
           "Unexpected continuation stack address space");
    return static_cast<ContStackAddrspace>(*AddrSpace);
  };

  static void setStackAddrspace(Module &M, ContStackAddrspace StackAddrspace) {
    auto *MD = M.getOrInsertNamedMetadata(MDStackAddrspaceName);
    MD->clearOperands();
    MD->addOperand(getI32MDConstant(M.getContext(),
                                    static_cast<uint32_t>(StackAddrspace)));
  }

  static std::optional<uint32_t> tryGetRtip(const Module &M) {
    auto *MD = M.getNamedMetadata(MDRtipName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void setRtip(Module &M, uint32_t RtipLevel) {
    auto *MD = M.getOrInsertNamedMetadata(MDRtipName);
    MD->clearOperands();
    MD->addOperand(getI32MDConstant(M.getContext(), RtipLevel));
  }

  static std::optional<uint32_t> tryGetFlags(const Module &M) {
    auto *MD = M.getNamedMetadata(MDFlagsName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void setFlags(Module &M, uint32_t Flags) {
    auto *MD = M.getOrInsertNamedMetadata(MDFlagsName);
    MD->clearOperands();
    MD->addOperand(getI32MDConstant(M.getContext(), Flags));
  }

  static void setContinuationStateByteCount(Function &F, uint32_t ByteCount) {
    F.setMetadata(MDStateName, getI32MDConstant(F.getContext(), ByteCount));
  }

  static std::optional<uint32_t>
  tryGetContinuationStateByteCount(const Function &F) {
    return extractZExtI32Constant(F.getMetadata(MDStateName));
  }

  static Type *getPayloadTypeFromMetadata(const Function &Func) {
    if (MDNode *Node = Func.getMetadata(MDContPayloadTyName))
      return getPayloadTypeFromMetadata(Node);

    report_fatal_error(Twine(MDContPayloadTyName) +
                       " metadata not found on function " + Func.getName() +
                       "!");
  }

  static Type *getPayloadTypeFromMetadata(const CallInst &CI) {
    if (MDNode *Node = CI.getMetadata(MDContPayloadTyName))
      return getPayloadTypeFromMetadata(Node);

    report_fatal_error(Twine(MDContPayloadTyName) +
                       " metadata not found on CallInst!");
  }

  static void setPayloadTypeMetadata(Instruction *I, Type *T) {
    I->setMetadata(ContHelper::MDContPayloadTyName,
                   MDNode::get(I->getContext(),
                               {ConstantAsMetadata::get(PoisonValue::get(T))}));
  }

  static bool isLgcCpsModule(Module &Mod) {
    return Mod.getNamedMetadata(MDLgcCpsModuleName) != nullptr;
  }

  // Specifies that an awaited call should wait on a wait mask.
  static void setIsWaitAwaitCall(CallInst &CI) {
    CI.setMetadata(ContHelper::MDIsWaitAwaitName,
                   MDTuple::get(CI.getContext(), {}));
  }

  // Queries whether an awaited call should wait on a wait mask.
  static bool isWaitAwaitCall(const CallInst &CI) {
    return CI.getMetadata(MDIsWaitAwaitName) != nullptr;
  }

  static void removeIsWaitAwaitMetadata(CallInst &CI) {
    CI.setMetadata(ContHelper::MDIsWaitAwaitName, nullptr);
  }

  /// Returns true if a call to the given function should be rematerialized
  /// in a shader of the specified kind.
  ///
  /// If no shader kind is specified, return false.
  static bool isRematerializableLgcRtOp(
      CallInst &CInst,
      std::optional<lgc::rt::RayTracingShaderStage> Kind = std::nullopt);

  static bool isLegacyEntryFunction(Function *Func) {
    return Func->hasMetadata(MDEntryName);
  }

  // Given a list of types, get a type that makes the list of types
  // occupy a specific number of dwords including it.
  static Type *getPaddingType(const DataLayout &DL, LLVMContext &Context,
                              ArrayRef<Type *> Types, unsigned TargetNumDwords);

  // Given a list of types, add a type to the list that makes the list of types
  // occupy a specific number of dwords.
  static void addPaddingType(const DataLayout &DL, LLVMContext &Context,
                             SmallVectorImpl<Type *> &Types,
                             unsigned TargetNumDwords);

  // Given a list of values, add a value to the list that makes the list of
  // values occupy a specific number of dwords.
  static void addPaddingValue(const DataLayout &DL, LLVMContext &Context,
                              SmallVectorImpl<Value *> &Values,
                              unsigned TargetNumDwords);

  // Returns whether the given flag is enabled in the given GpuRt module,
  // using the GpuRt version flags intrinsic. If the intrinsic is not found,
  // returns true, enabling new behavior (e.g. for tests).
  static bool getGpurtVersionFlag(Module &GpurtModule, GpuRtVersionFlag Flag);

  // Handles _AmdGetSetting_* intrinsics.
  static void handleGetSetting(Function &F, ArrayRef<ContSetting> Settings);
};

class ShaderStageHelper final {
public:
  static DXILShaderKind
  rtShaderStageToDxilShaderKind(lgc::rt::RayTracingShaderStage Stage) {
    switch (Stage) {
    case lgc::rt::RayTracingShaderStage::RayGeneration:
      return DXILShaderKind::RayGeneration;
    case lgc::rt::RayTracingShaderStage::Intersection:
      return DXILShaderKind::Intersection;
    case lgc::rt::RayTracingShaderStage::AnyHit:
      return DXILShaderKind::AnyHit;
    case lgc::rt::RayTracingShaderStage::ClosestHit:
      return DXILShaderKind::ClosestHit;
    case lgc::rt::RayTracingShaderStage::Miss:
      return DXILShaderKind::Miss;
    case lgc::rt::RayTracingShaderStage::Callable:
      return DXILShaderKind::Callable;
    case lgc::rt::RayTracingShaderStage::KernelEntry:
    case lgc::rt::RayTracingShaderStage::Traversal:
      // TODO: Migrate to an enum shared by GpuRt HLSL and the compiler C++
      //       source that explicitly supports KernelEntry and Traversal,
      //       eliminate most uses of DXILShaderKind except for initial
      //       conversions to the shared enum.
      return DXILShaderKind::Compute;
    default:
      llvm_unreachable("invalid stage!");
    }
  }

  static std::optional<lgc::rt::RayTracingShaderStage>
  dxilShaderKindToRtShaderStage(DXILShaderKind Kind) {
    switch (Kind) {
    case DXILShaderKind::RayGeneration:
      return lgc::rt::RayTracingShaderStage::RayGeneration;
    case DXILShaderKind::Intersection:
      return lgc::rt::RayTracingShaderStage::Intersection;
    case DXILShaderKind::AnyHit:
      return lgc::rt::RayTracingShaderStage::AnyHit;
    case DXILShaderKind::ClosestHit:
      return lgc::rt::RayTracingShaderStage::ClosestHit;
    case DXILShaderKind::Miss:
      return lgc::rt::RayTracingShaderStage::Miss;
    case DXILShaderKind::Callable:
      return lgc::rt::RayTracingShaderStage::Callable;
    default:
      return std::nullopt;
    }
  }
};

namespace ContDriverFunc {
#define DRIVER_FUNC_NAME(KEY) constexpr const char *KEY##Name = "_cont_" #KEY;
DRIVER_FUNC_NAME(GetContinuationStackGlobalMemBase)
DRIVER_FUNC_NAME(GetTriangleHitAttributes)
DRIVER_FUNC_NAME(SetTriangleHitAttributes)
DRIVER_FUNC_NAME(GetCandidateState)
DRIVER_FUNC_NAME(GetCommittedState)
DRIVER_FUNC_NAME(GetContinuationStackAddr)
DRIVER_FUNC_NAME(SetupRayGen)
DRIVER_FUNC_NAME(ExitRayGen)
DRIVER_FUNC_NAME(IsEndSearch)
DRIVER_FUNC_NAME(GetLocalRootIndex)
DRIVER_FUNC_NAME(SetLocalRootIndex)
DRIVER_FUNC_NAME(TraceRay)
DRIVER_FUNC_NAME(CallShader)
DRIVER_FUNC_NAME(ReportHit)
DRIVER_FUNC_NAME(AcceptHit)
DRIVER_FUNC_NAME(GetSbtAddress)
DRIVER_FUNC_NAME(GetSbtStride)
DRIVER_FUNC_NAME(HitKind)
DRIVER_FUNC_NAME(Traversal)
DRIVER_FUNC_NAME(KernelEntry)
DRIVER_FUNC_NAME(GpurtVersionFlags)
DRIVER_FUNC_NAME(ShaderStart)

#undef DRIVER_FUNC_NAME
} // namespace ContDriverFunc

/// Free-standing helpers.

// Helper to visit all calls of a function.
// Expected type for Callback:
//  void(CallInst &)
template <typename CallbackTy>
void forEachCall(Function &F, CallbackTy Callback) {
  static_assert(std::is_invocable_v<CallbackTy, CallInst &>);
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser()))
      if (CInst->isCallee(&Use))
        Callback(*CInst);
  }
}

// Replace all calls to a given function with some value.
// Removes the original call.
void replaceCallsToFunction(llvm::Function &F, llvm::Value &Replacement);

bool isLgcRtOp(const llvm::Function *F);

// Move all basic blocks of OldFunc to NewFunc.
void moveFunctionBody(Function &OldFunc, Function &NewFunc);

// From a specific lgc.rt call operation, try to find information about the
// corresponding GPURT implementation.
std::optional<GpuRtIntrinsicEntry>
findIntrImplEntryByIntrinsicCall(CallInst *Call);

// Collect and remove unused function declarations.
// @OnlyIntrinsics is used to differentiate whether all function declarations
// shall or only declarations for lgc.rt or dx.op intrinsics shall be removed.
// This is because we are not linking the actual GPURT runtime in the
// continuations lit tests but only define a stub for these driver functions.
// Additionally, calls to several rematerializable operations are only inserted
// during DXILContPostProcess, so we cannot remove all unused declarations right
// at the end of LowerRaytracingPipeline.
bool removeUnusedFunctionDecls(Module *Mod, bool OnlyIntrinsics = true);

// For each basic block in Func, find the terminator. If it is contained in
// TerminatorOpcodes, then apply the callback on the terminator.
template <typename CallbackTy, typename = std::enable_if<std::is_invocable_v<
                                   CallbackTy, llvm::Instruction &>>>
void forEachTerminator(Function *Func, ArrayRef<unsigned> TerminatorOpcodes,
                       CallbackTy Callback) {
  for (auto &BB : *Func) {
    auto *Terminator = BB.getTerminator();
    if (llvm::find(TerminatorOpcodes, Terminator->getOpcode()) !=
        TerminatorOpcodes.end())
      Callback(*Terminator);
  }
}

// Do store-to-load forwarding for memory access to continuation stack.  This is
// helpful to mitigate the issue that coroutine passes in some cases still load
// state from the in-memory continuation state when it is still available in SSA
// variables. The implementation is assuming there is no other pointers in the
// program that may alias the pointer argument.
void forwardContinuationFrameStoreToLoad(DominatorTree &DT, Value *FramePtr);

/// Look for the continue call that is dominated by the call to
/// GetResumePointAddr. Due to saving the payload before, many basic blocks may
/// have been inserted, traverse them while making sure that this
/// GetResumePointAddr is the only possible predecessor.
std::optional<CallInst *> findDominatedContinueCall(CallInst *GetResPointAddr);
} // namespace llvm
