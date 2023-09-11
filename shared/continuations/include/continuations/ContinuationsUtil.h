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

//===- ContinuationsUtil.h - Continuations utility header -----------------===//
//
// This file declares helper classes and functions for continuation passes.
//
//===----------------------------------------------------------------------===//

#ifndef CONTINUATIONS_CONTINUATIONS_UTIL_H
#define CONTINUATIONS_CONTINUATIONS_UTIL_H

#include "llvm-dialects/Dialect/OpDescription.h"
#include "llvm-dialects/TableGen/Operations.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace DialectUtils {

llvm::StringRef getLgcRtDialectOpName(llvm::StringRef FullName);

bool isLgcRtOp(const llvm::Function *F);

template <typename OpT> bool isDialectOpDeclaration(llvm::Function &F) {
  static llvm_dialects::OpDescription Desc =
      llvm_dialects::OpDescription::get<OpT>();
  return Desc.matchDeclaration(F);
}

template <typename... OpTypes>
bool isAnyDialectOpDeclaration(llvm::Function &F) {
  return (isDialectOpDeclaration<OpTypes>(F) || ...);
}

template <typename... OpTypes>
bool isNoneOfDialectOpDeclaration(llvm::Function &F) {
  return (!isDialectOpDeclaration<OpTypes>(F) && ...);
}
} // namespace DialectUtils

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
/// Amount of registers reserved for the continuation state.
/// Spill everything into memory. No explicit memory address needed, which is
/// instead derived from the CSP.
const unsigned ContinuationStateRegisterCount = 0;
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
/// The minimum size for the pre-allocated continuation state is the size of a
/// pointer.
const unsigned MinimumContinuationStateBytes = 8;

struct DxRayIntrinsic {
  unsigned int Id;
  StringRef Name;
};

struct GpuRtIntrinsicEntry {
  StringRef Name;
  bool AccessesHitData = false;
};

extern StringMap<GpuRtIntrinsicEntry> LgcRtGpuRtMap;

// This must match DXIL::ShaderKind from DxilConstants.h, and also
// DXILShaderKind in a matching definition in GPURT, because it is used
// as return type of an intrinsic.
enum class DXILShaderKind : uint32_t {
  Pixel = 0,
  Vertex,
  Geometry,
  Hull,
  Domain,
  Compute,
  Library,
  RayGeneration,
  Intersection,
  AnyHit,
  ClosestHit,
  Miss,
  Callable,
  Mesh,
  Amplification,
  Node,
  Invalid
};

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
class DXILContArgTy {
private:
  Type *ArgTy;
  Type *ElemTy;

public:
  DXILContArgTy() : ArgTy(nullptr), ElemTy(nullptr) {}
  DXILContArgTy(Type *Arg, Type *Elem) : ArgTy(Arg), ElemTy(Elem) {}
  DXILContArgTy(Type *Arg);

  static DXILContArgTy get(const Function *F, const Argument *Arg);
  static DXILContArgTy get(const Function *F, const unsigned ArgNo);
  static DXILContArgTy get(const Metadata *MD, LLVMContext &Context);

  Type *asType(LLVMContext &Context);
  Type *getPointerElementType() const;

  bool isPointerTy() const;
  bool isVoidTy() const;
  Metadata *getTypeMetadata(LLVMContext &Context);

  bool operator==(const DXILContArgTy &RHS) const {
    return (ArgTy == RHS.ArgTy) && (ElemTy == RHS.ElemTy);
  }
};

// Helper class to abstract over function types.
// Uses DXILContArgTy to derive types from and encode types to custom metadata.
class DXILContFuncTy {
public:
  DXILContFuncTy() {}
  DXILContFuncTy(DXILContArgTy Return) : ReturnTy(Return) {}
  DXILContFuncTy(DXILContArgTy Return, ArrayRef<DXILContArgTy> Args)
      : ReturnTy(Return), ArgTys(Args) {}

  DXILContArgTy ReturnTy;
  SmallVector<DXILContArgTy> ArgTys;

  static DXILContFuncTy get(const FunctionType *FuncTy);
  static DXILContFuncTy get(const Function *F);
  static DXILContFuncTy get(const Metadata *MD, LLVMContext &Context);

  FunctionType *asFunctionType(LLVMContext &Context);
  void writeMetadata(Function *F);
};

// Helper class to access data specific to DXIL continuation passes, e.g.
// metadata or globals.
class DXILContHelper {
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
  // MinPayloadRegisterCount:
  // The minimum required number of payload registers, which is used to pass
  // inter-module data into a module. Ensures a minimum size of the generated
  // payload global, and thereby the payload size used to annotate Traversal and
  // when lowering payload access intrinsics. This relies on the relevant copy
  // of Traversal being processed last, after all app shader modules. This value
  // is *not* guaranteed to be sufficiently large to account for payloads
  // occurring in other pipelines that currently compiled shaders may be used
  // with. Thus, we currently do *not* use it to annotate Intersection shaders.
  static constexpr const char *MDMinPayloadRegisterCountName =
      "continuation.minPayloadRegisterCount";
  // The maximum allowed number of payload registers to be used for payload and
  // other inter-stage date (e.g. attributes). If state does not fit into this
  // limit, we spill to the continuation stack.
  static constexpr const char *MDMaxPayloadRegisterCountName =
      "continuation.maxPayloadRegisterCount";
  static constexpr const char *MDStackAddrspaceName =
      "continuation.stackAddrspace";

  // Function-scope metadata for payload and hit attribute size limits,
  // referring to the app-defined structs only.
  static constexpr const char *MDMaxHitAttributeBytesName =
      "continuation.maxHitAttributeBytes";
  static constexpr const char *MDMaxPayloadBytesName =
      "continuation.maxPayloadBytes";

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
  static constexpr const char *MDDXILPayloadTyName = "dxil.payload.type";

  // Global variable names
  static constexpr const char *GlobalPayloadName = "PAYLOAD";
  static constexpr const char *GlobalContStateName = "CONTINUATION_STATE";
  static constexpr const char *GlobalRegistersName = "REGISTERS";

  static void RegisterPasses(llvm::PassBuilder &PB, bool NeedDialectContext);

  // Registers the generic Continuation pipeline to a LLVM Module Pass manager.
  static void addContinuationPasses(llvm::ModulePassManager &MPM);

  // Registers the DXIL-specific Continuation pipeline to a LLVM Module Pass
  // manager.
  static void addDxilContinuationPasses(llvm::ModulePassManager &MPM);

  // Set metadata specifying the number of outgoing payload registers.
  static void setOutgoingRegisterCount(Instruction *I, uint32_t RegisterCount) {
    I->setMetadata(MDRegisterCountName,
                   getI32MDConstant(I->getContext(), RegisterCount));
  }

  // Get the number of incoming payload registers if set.
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
  tryGetMinPayloadRegisterCount(const Module &M) {
    auto *MD = M.getNamedMetadata(MDMinPayloadRegisterCountName);
    if (!MD)
      return {};
    return extractZExtI32Constant(MD->getOperand(0));
  };

  static void setMinPayloadRegisterCount(Module &M,
                                         uint32_t MinPayloadRegisterCount) {
    auto *MD = M.getOrInsertNamedMetadata(MDMinPayloadRegisterCountName);
    assert(MD && "Failed to create metadata node!");
    MD->clearOperands();
    MD->addOperand(getI32MDConstant(M.getContext(), MinPayloadRegisterCount));
  }

  // If there is module-level metadata specifying the maximum number
  // of payload registers, return that value. Otherwise, return std::nullopt.
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

  // Returns the number of payload registers used in this module.
  // Only available after having finished continuation passes.
  static std::optional<uint32_t> tryGetPayloadRegisterCount(const Module &M) {
    auto *Registers = M.getGlobalVariable(GlobalRegistersName);
    if (!Registers)
      return {};
    const uint32_t NumRegistersI32s =
        Registers->getValueType()->getArrayNumElements();
    assert(NumRegistersI32s >= ContinuationStateRegisterCount);
    const uint32_t NumPayloadRegistersI32s =
        NumRegistersI32s - ContinuationStateRegisterCount;
    assert(NumPayloadRegistersI32s >=
           tryGetMinPayloadRegisterCount(M).value_or(NumPayloadRegistersI32s));
    assert(NumPayloadRegistersI32s <=
           tryGetMaxPayloadRegisterCount(M).value_or(NumPayloadRegistersI32s));
    return NumPayloadRegistersI32s;
  }

  static void setMaxHitAttributeByteCount(Function &F,
                                          uint32_t MaxHitAttributeByteCount) {
    F.setMetadata(MDMaxHitAttributeBytesName,
                  getI32MDConstant(F.getContext(), MaxHitAttributeByteCount));
  }

  static std::optional<uint32_t>
  tryGetMaxHitAttributeByteCount(const Function &F) {
    return extractZExtI32Constant(F.getMetadata(MDMaxHitAttributeBytesName));
  }

  static void setMaxPayloadByteCount(Function &F,
                                     uint32_t MaxPayloadByteCount) {
    F.setMetadata(MDMaxPayloadBytesName,
                  getI32MDConstant(F.getContext(), MaxPayloadByteCount));
  }

  static std::optional<uint32_t> tryGetMaxPayloadByteCount(const Function &F) {
    return extractZExtI32Constant(F.getMetadata(MDMaxPayloadBytesName));
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

  static void setContinuationStateByteCount(Function &F, uint32_t ByteCount) {
    F.setMetadata(MDStateName, getI32MDConstant(F.getContext(), ByteCount));
  }

  static std::optional<uint32_t>
  tryGetContinuationStateByteCount(const Function &F) {
    return extractZExtI32Constant(F.getMetadata(MDStateName));
  }

  static Function *getAliasedFunction(Module &M, StringRef Name) {
    llvm::Constant *FuncOrAlias = M.getNamedValue(Name);
    if (!FuncOrAlias)
      return nullptr;
    while (auto *Alias = dyn_cast<GlobalAlias>(FuncOrAlias))
      FuncOrAlias = Alias->getAliasee();
    return dyn_cast<Function>(FuncOrAlias);
  }

  static bool isTraversal(Function &F) {
    // TODO: Make this more robust somehow, restricting to library functions.
    return F.getName().contains("Traversal");
  }

  static Type *getPayloadTypeFromMetadata(const Function &Func) {
    if (MDNode *Node = Func.getMetadata(MDDXILPayloadTyName))
      return getPayloadTypeFromMetadata(Node);

    report_fatal_error(Twine(MDDXILPayloadTyName) +
                       " metadata not found on function " + Func.getName() +
                       "!");
  }

  static Type *getPayloadTypeFromMetadata(const CallInst &CI) {
    if (MDNode *Node = CI.getMetadata(MDDXILPayloadTyName))
      return getPayloadTypeFromMetadata(Node);

    report_fatal_error(Twine(MDDXILPayloadTyName) +
                       " metadata not found on CallInst!");
  }
};

/// Free-standing helpers.

// A little helper function that allows to apply a callback on the users (calls)
// of a function.
void forEachCall(Function &F, const std::function<void(CallInst &)> &Callback);

// A little helper function that allows to apply a callback on the users (calls)
// of a set of functions given by iterating over a module.
void forEachCall(Module &M, const std::function<void(CallInst &)> &Callback);

// A little helper function that allows to apply a callback on the users (calls)
// of a set of functions.
void forEachCall(ArrayRef<Function *> Funcs,
                 const std::function<void(CallInst &)> &Callback);

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
} // namespace llvm

#endif
