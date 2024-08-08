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

//===- Continuations.cpp - Continuations utilities ------------------------===//
//
// This file defines implementations for helper functions for continuation
// passes.
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm-dialects/Dialect/OpSet.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntervalTree.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Coroutines/CoroCleanup.h"
#include "llvm/Transforms/Coroutines/CoroEarly.h"
#include "llvm/Transforms/Coroutines/CoroElide.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/FixIrreducible.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

#define DEBUG_TYPE "continuations"

using namespace llvm;

#define GPURTMAP_ENTRY(Op, GpurtName, AccessesHitData)                                                                 \
  {                                                                                                                    \
    llvm_dialects::OpDescription::get<lgc::rt::Op>(), { GpurtName, AccessesHitData }                                   \
  }

const llvm_dialects::OpMap<llvm::GpuRtIntrinsicEntry> llvm::LgcRtGpuRtMap = {{
    GPURTMAP_ENTRY(InstanceIdOp, "InstanceID", true),
    GPURTMAP_ENTRY(InstanceIndexOp, "InstanceIndex", true),
    GPURTMAP_ENTRY(HitKindOp, "HitKind", true),
    GPURTMAP_ENTRY(RayFlagsOp, "RayFlags", false),
    GPURTMAP_ENTRY(DispatchRaysIndexOp, "DispatchRaysIndex3", false),
    GPURTMAP_ENTRY(DispatchRaysDimensionsOp, "DispatchRaysDimensions3", false),
    GPURTMAP_ENTRY(WorldRayOriginOp, "WorldRayOrigin3", false),
    GPURTMAP_ENTRY(WorldRayDirectionOp, "WorldRayDirection3", false),
    GPURTMAP_ENTRY(ObjectRayOriginOp, "ObjectRayOrigin3", true),
    GPURTMAP_ENTRY(ObjectRayDirectionOp, "ObjectRayDirection3", true),
    GPURTMAP_ENTRY(ObjectToWorldOp, "ObjectToWorld4x3", true),
    GPURTMAP_ENTRY(WorldToObjectOp, "WorldToObject4x3", true),
    GPURTMAP_ENTRY(RayTminOp, "RayTMin", false),
    GPURTMAP_ENTRY(RayTcurrentOp, "RayTCurrent", true),
    GPURTMAP_ENTRY(IgnoreHitOp, "IgnoreHit", false),
    GPURTMAP_ENTRY(AcceptHitAndEndSearchOp, "AcceptHitAndEndSearch", false),
    GPURTMAP_ENTRY(TraceRayOp, "TraceRay", false),
    GPURTMAP_ENTRY(ReportHitOp, "ReportHit", false),
    GPURTMAP_ENTRY(CallCallableShaderOp, "CallShader", false),
    GPURTMAP_ENTRY(PrimitiveIndexOp, "PrimitiveIndex", true),
    GPURTMAP_ENTRY(GeometryIndexOp, "GeometryIndex", true),
    GPURTMAP_ENTRY(InstanceInclusionMaskOp, "InstanceInclusionMask", false),
    GPURTMAP_ENTRY(TriangleVertexPositionsOp, "TriangleVertexPositions", true),
}};

#undef GPURTMAP_ENTRY

void llvm::replaceCallsToFunction(Function &F, Value &Replacement) {
  llvm::forEachCall(F, [&](CallInst &CInst) {
    // Basic sanity check. We should also check for dominance.
    assert((!isa<Instruction>(&Replacement) || cast<Instruction>(&Replacement)->getFunction() == CInst.getFunction()) &&
           "llvm::replaceCallsToFunction: Replacement should "
           "reside in the same function as CallInst to replace!");
    CInst.replaceAllUsesWith(&Replacement);
    CInst.eraseFromParent();
  });
}

bool llvm::isLgcRtOp(const llvm::Function *F) {
  return F && F->getName().starts_with("lgc.rt.");
}

void llvm::moveFunctionBody(Function &OldFunc, Function &NewFunc) {
  while (!OldFunc.empty()) {
    BasicBlock *BB = &OldFunc.front();
    BB->removeFromParent();
    BB->insertInto(&NewFunc);
  }
}

std::optional<llvm::GpuRtIntrinsicEntry> llvm::findIntrImplEntryByIntrinsicCall(CallInst *Call) {
  if (!isLgcRtOp(Call->getCalledFunction()))
    return std::nullopt;

  auto ImplEntry = LgcRtGpuRtMap.find(*Call);
  if (ImplEntry == LgcRtGpuRtMap.end())
    report_fatal_error("Unhandled lgc.rt op!");

  return *ImplEntry.val();
}

bool llvm::removeUnusedFunctionDecls(Module *Mod, bool OnlyIntrinsics) {
  bool DidChange = false;

  for (Function &F : make_early_inc_range(*Mod)) {
    if (F.isDeclaration() && F.user_empty()) {
      if (!OnlyIntrinsics || (isLgcRtOp(&F) || F.getName().starts_with("dx.op."))) {
        F.eraseFromParent();
        DidChange = true;
      }
    }
  }

  return DidChange;
}

bool ContHelper::isRematerializableLgcRtOp(CallInst &CInst, std::optional<lgc::rt::RayTracingShaderStage> Kind) {
  using namespace lgc::rt;
  Function *Callee = CInst.getCalledFunction();
  if (!llvm::isLgcRtOp(Callee))
    return false;

  // Always rematerialize
  static const llvm_dialects::OpSet RematerializableDialectOps =
      llvm_dialects::OpSet::get<DispatchRaysDimensionsOp, DispatchRaysIndexOp>();
  if (RematerializableDialectOps.contains(*Callee))
    return true;

  // Rematerialize for Intersection that can only call ReportHit, which keeps
  // the largest system data struct. These cannot be rematerialized in
  // ClosestHit, because if ClosestHit calls TraceRay or CallShader, that
  // information is lost from the system data struct. Also exclude rayTCurrent
  // because ReportHit calls can change that.
  if (!Kind || *Kind == RayTracingShaderStage::Intersection) {
    static const llvm_dialects::OpSet RematerializableIntersectionDialectOps =
        llvm_dialects::OpSet::get<InstanceIdOp, InstanceIndexOp, GeometryIndexOp, ObjectRayDirectionOp,
                                  ObjectRayOriginOp, ObjectToWorldOp, PrimitiveIndexOp, RayFlagsOp, RayTminOp,
                                  WorldRayDirectionOp, WorldRayOriginOp, WorldToObjectOp, InstanceInclusionMaskOp>();
    if (RematerializableIntersectionDialectOps.contains(*Callee))
      return true;
  }

  return false;
}

Type *ContHelper::getPaddingType(const DataLayout &DL, LLVMContext &Context, ArrayRef<Type *> Types,
                                 unsigned TargetNumDwords) {
  unsigned DwordsOccupied = lgc::cps::getArgumentDwordCount(DL, Types);

  assert(DwordsOccupied <= TargetNumDwords);
  unsigned DwordsRemaining = TargetNumDwords - DwordsOccupied;
  if (DwordsRemaining > 0)
    return ArrayType::get(Type::getInt32Ty(Context), DwordsRemaining);

  return StructType::get(Context);
}

void ContHelper::addPaddingType(const DataLayout &DL, LLVMContext &Context, SmallVectorImpl<Type *> &Types,
                                unsigned TargetNumDwords) {
  Types.push_back(getPaddingType(DL, Context, Types, TargetNumDwords));
}

void ContHelper::addPaddingValue(const DataLayout &DL, LLVMContext &Context, SmallVectorImpl<Value *> &Values,
                                 unsigned TargetNumDwords) {
  SmallVector<Type *> Types;
  for (auto Value : Values)
    Types.push_back(Value->getType());

  Values.push_back(PoisonValue::get(getPaddingType(DL, Context, Types, TargetNumDwords)));
}

bool ContHelper::getGpurtVersionFlag(Module &GpurtModule, GpuRtVersionFlag Flag) {
  auto *F = GpurtModule.getFunction(ContDriverFunc::GpurtVersionFlagsName);
  if (!F) {
    // If the GpuRt version flags intrinsic is not found, treat flags as set,
    // enabling new behavior. This is mainly intended for tests which lack the
    // intrinsic and should always use the new behavior.
    return true;
  }
  StructType *RetTy = cast<StructType>(F->getReturnType());
  assert(RetTy->getNumElements() == 1);
  ArrayType *InnerTy = cast<ArrayType>(RetTy->getElementType(0));
  uint32_t Flags = InnerTy->getNumElements();
  return (Flags & static_cast<uint32_t>(Flag)) != 0;
}

void llvm::forwardContinuationFrameStoreToLoad(DominatorTree &DT, Value *FramePtr) {
  assert(FramePtr);

  DenseMap<int64_t, SmallVector<LoadInst *>> OffsetLoadMap;
  using StoreIntervalTree = IntervalTree<int64_t, StoreInst *>;
  using IntervalTreeData = StoreIntervalTree::DataType;
  StoreIntervalTree::Allocator Allocator;
  StoreIntervalTree StoreIntervals(Allocator);
  // While IntervalTree is efficient at answering which store would write to
  // memory that fully cover the memory range that will be loaded [load_begin,
  // load_end] by detecting the intervals that have intersection with both
  // `load_begin` and `load_end`, but it is not good at answering whether there
  // are stores that are strictly within the range (load_begin, load_end). So
  // we introduce a sorted array to help detecting if there is conflicting
  // store within the range (load_begin, load_end).
  struct OffsetStorePair {
    OffsetStorePair(int64_t Offset, StoreInst *Store) : Offset(Offset), Store(Store) {}
    int64_t Offset;
    StoreInst *Store;
  };
  SmallVector<OffsetStorePair> SortedStores;

  struct PointerUse {
    PointerUse(Use *P, int64_t O) : Ptr(P), Offset(O) {}
    // The Use of a particular pointer to be visited.
    Use *Ptr;
    // The byte offset to the base pointer.
    int64_t Offset;
  };
  SmallVector<PointerUse> Worklist;
  for (auto &U : FramePtr->uses())
    Worklist.push_back(PointerUse(&U, 0));

  while (!Worklist.empty()) {
    PointerUse PtrUse = Worklist.pop_back_val();
    User *U = PtrUse.Ptr->getUser();
    switch (cast<Instruction>(U)->getOpcode()) {
    case Instruction::GetElementPtr: {
      auto *Gep = cast<GetElementPtrInst>(U);
      const DataLayout &DL = Gep->getModule()->getDataLayout();
      unsigned OffsetBitWidth = DL.getIndexSizeInBits(Gep->getAddressSpace());
      APInt Offset(OffsetBitWidth, 0);
      bool ConstantOffset = Gep->accumulateConstantOffset(Gep->getModule()->getDataLayout(), Offset);
      // Give up on dynamic indexes for simplicity.
      if (!ConstantOffset)
        return;

      for (auto &UU : Gep->uses())
        Worklist.push_back(PointerUse(&UU, Offset.getSExtValue() + PtrUse.Offset));
      break;
    }
    case Instruction::Load: {
      auto *Load = cast<LoadInst>(U);
      if (!Load->isSimple())
        return;
      SmallVector<LoadInst *> &Instrs = OffsetLoadMap[PtrUse.Offset];
      Instrs.push_back(cast<LoadInst>(U));
      break;
    }
    case Instruction::Store: {
      auto *Store = cast<StoreInst>(U);
      if (!Store->isSimple() || Store->getValueOperand() == PtrUse.Ptr->get())
        return;

      assert(Store->getPointerOperand() == PtrUse.Ptr->get());
      const DataLayout &DL = Store->getModule()->getDataLayout();
      unsigned StoredBytes = DL.getTypeStoreSize(Store->getValueOperand()->getType());

      SortedStores.push_back(OffsetStorePair(PtrUse.Offset, Store));
      StoreIntervals.insert(PtrUse.Offset, PtrUse.Offset + StoredBytes - 1, Store);
      break;
    }
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast: {
      for (auto &UU : cast<Instruction>(U)->uses())
        Worklist.push_back(PointerUse(&UU, PtrUse.Offset));
      break;
    }

    case Instruction::Call: {
      auto *Call = cast<CallInst>(U);
      // Ignore lifetime markers.
      if (Call->isLifetimeStartOrEnd())
        break;
    }
      LLVM_FALLTHROUGH;
    default:
      LLVM_DEBUG(dbgs() << "Unhandled user of continuation frame pointer: " << *U << '\n');
      return;
    }
  }

  StoreIntervals.create();
  llvm::sort(SortedStores,
             [](const OffsetStorePair &Left, const OffsetStorePair &Right) { return Left.Offset < Right.Offset; });

  // Nothing to do if there is no store.
  if (StoreIntervals.empty())
    return;

  for (const auto &[Offset, Loads] : OffsetLoadMap) {
    assert(!Loads.empty());
    auto IntersectionsLeft = StoreIntervals.getContaining(Offset);
    // Nothing to do if there is no store or more than one store.
    if (IntersectionsLeft.size() != 1)
      continue;

    const IntervalTreeData &StoreInfo = *IntersectionsLeft.front();
    // The load and store are at different addresses, abort. This can be
    // improved later.
    if (Offset != StoreInfo.left())
      continue;

    for (auto *Load : Loads) {
      const DataLayout &DL = Load->getModule()->getDataLayout();
      unsigned LoadBytes = DL.getTypeStoreSize(Load->getType());
      auto IntersectionsRight = StoreIntervals.getContaining(Offset + LoadBytes - 1);
      assert(!IntersectionsRight.empty());
      // Make sure the store we found fully covers the loaded range and is the
      // only one.
      if (IntersectionsRight.size() != 1 || IntersectionsRight.front()->value() != StoreInfo.value())
        continue;

      StoreInst *Store = StoreInfo.value();
      // Get the first iterator pointing to a value that is strictly greater
      // than Offset.
      auto *MaybeConflict = llvm::upper_bound(SortedStores, Offset,
                                              [](int64_t V, const OffsetStorePair &Elem) { return V < Elem.Offset; });
      // Abort if there is another store which write to the memory region
      // strictly within the loaded region.
      if (MaybeConflict != SortedStores.end() && MaybeConflict->Offset < StoreInfo.right())
        continue;

      // Currently we only forward if the value types are the same. This can
      // be improved.
      Type *StoredTy = Store->getValueOperand()->getType();
      if (Load->getType() != StoredTy)
        continue;
      if (!DT.dominates(Store, Load))
        continue;

      auto *LoadPtr = Load->getPointerOperand();
      Load->replaceAllUsesWith(Store->getValueOperand());
      Load->eraseFromParent();

      // Erase the possibly dead instruction which defines the pointer.
      if (!LoadPtr->use_empty())
        continue;
      if (auto *PtrInstr = dyn_cast<Instruction>(LoadPtr))
        PtrInstr->eraseFromParent();
    }
  }
}

static const char *toString(DXILShaderKind ShaderKind) {
  switch (ShaderKind) {
  case DXILShaderKind::Pixel:
    return "pixel";
  case DXILShaderKind::Vertex:
    return "vertex";
  case DXILShaderKind::Geometry:
    return "geometry";
  case DXILShaderKind::Hull:
    return "hull";
  case DXILShaderKind::Domain:
    return "domain";
  case DXILShaderKind::Compute:
    return "compute";
  case DXILShaderKind::Library:
    return "library";
  case DXILShaderKind::RayGeneration:
    return "raygeneration";
  case DXILShaderKind::Intersection:
    return "intersection";
  case DXILShaderKind::AnyHit:
    return "anyhit";
  case DXILShaderKind::ClosestHit:
    return "closesthit";
  case DXILShaderKind::Miss:
    return "miss";
  case DXILShaderKind::Callable:
    return "callable";
  case DXILShaderKind::Mesh:
    return "mesh";
  case DXILShaderKind::Amplification:
    return "amplification";
  case DXILShaderKind::Node:
    return "node";
  case DXILShaderKind::Invalid:
    return "invalid";
  }
  report_fatal_error("unexpected shader kind");
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Str, DXILShaderKind ShaderKind) {
  Str << ::toString(ShaderKind);
  return Str;
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Str, lgc::rt::RayTracingShaderStage Stage) {
  Str << ::toString(ShaderStageHelper::rtShaderStageToDxilShaderKind(Stage));
  return Str;
}

void ContHelper::RegisterPasses(PassBuilder &PB, bool NeedDialectContext) {
#define HANDLE_PASS(NAME, CREATE_PASS)                                                                                 \
  if (innerPipeline.empty() && name == NAME) {                                                                         \
    passMgr.addPass(CREATE_PASS);                                                                                      \
    return true;                                                                                                       \
  }

#define HANDLE_ANALYSIS(NAME, CREATE_PASS, IRUNIT)                                                                     \
  if (innerPipeline.empty() && name == "require<" NAME ">") {                                                          \
    passMgr.addPass(RequireAnalysisPass<std::remove_reference_t<decltype(CREATE_PASS)>, IRUNIT>());                    \
    return true;                                                                                                       \
  }                                                                                                                    \
  if (innerPipeline.empty() && name == "invalidate<" NAME ">") {                                                       \
    passMgr.addPass(InvalidateAnalysisPass<std::remove_reference_t<decltype(CREATE_PASS)>>());                         \
    return true;                                                                                                       \
  }

  PB.registerPipelineParsingCallback(
      [](StringRef name, ModulePassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_MODULE_PASS HANDLE_PASS
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS) HANDLE_ANALYSIS(NAME, CREATE_PASS, Module)
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &PassMgr, ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_FUNCTION_PASS HANDLE_PASS
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef Name, LoopPassManager &PassMgr, ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_LOOP_PASS HANDLE_PASS
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef name, ModulePassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_CGSCC_PASS(NAME, CREATE_PASS)                                                                             \
  if (innerPipeline.empty() && name == NAME) {                                                                         \
    passMgr.addPass(createModuleToPostOrderCGSCCPassAdaptor(CREATE_PASS));                                             \
    return true;                                                                                                       \
  }
#include "PassRegistry.inc"
        return false;
      });

#undef HANDLE_ANALYSIS
#undef HANDLE_PASS

  PB.registerAnalysisRegistrationCallback([=](ModuleAnalysisManager &AnalysisManager) {
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS) AnalysisManager.registerPass([&] { return CREATE_PASS; });
#include "PassRegistry.inc"
  });

  auto *PIC = PB.getPassInstrumentationCallbacks();
  if (PIC) {
#define CONT_PASS(NAME, CREATE_PASS) PIC->addClassToPassName(decltype(CREATE_PASS)::name(), NAME);
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS) PIC->addClassToPassName(decltype(CREATE_PASS)::name(), NAME);
#include "PassRegistry.inc"
  }
}

void ContHelper::addContinuationPasses(ModulePassManager &MPM) {
  // Inline functions into shaders, so everything is in a shader
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));

  MPM.addPass(LowerRaytracingPipelinePass());

  // Convert the system data struct to a value, so it isn't stored in the
  // continuation state
  MPM.addPass(createModuleToFunctionPassAdaptor(SROAPass(llvm::SROAOptions::ModifyCFG)));
  MPM.addPass(LowerAwaitPass());

  MPM.addPass(CoroEarlyPass());
  MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(DXILCoroSplitPass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(CoroElidePass()));
  MPM.addPass(CoroCleanupPass());

  MPM.addPass(LegacyCleanupContinuationsPass());
  MPM.addPass(ContinuationsStatsReportPass());
  MPM.addPass(DXILContPostProcessPass());

#ifndef NDEBUG
  MPM.addPass(ContinuationsLintPass());
#endif

  // The FixIrreducible pass does not cope with switch instructions, so lower
  // them before.
  MPM.addPass(createModuleToFunctionPassAdaptor(LowerSwitchPass()));

  // Splitting functions as part of LLVM's coroutine transformation can lead
  // to irreducible resume functions in some cases. Use the FixIrreduciblePass
  // to resolve the irreducibility with a dynamic dispatch block. In the future
  // we might want to use node splitting instead for better perf, or a
  // combination of the two. Note: Even if the control flow is reducible, this
  // pass can still change the module in its preprocessing, lowering switches to
  // chained ifs.
  MPM.addPass(createModuleToFunctionPassAdaptor(FixIrreduciblePass()));
}

void ContHelper::addDxilContinuationPasses(ModulePassManager &MPM, Module *GpurtLibrary) {
  if (GpurtLibrary) {
    // Set up GpurtContext so that later passes can access the library via it.
    auto &GpurtContext = lgc::GpurtContext::get(GpurtLibrary->getContext());
    GpurtContext.theModule = GpurtLibrary;
  }

  MPM.addPass(DXILContPreHookPass());

  // Translate dx.op intrinsic calls to lgc.rt dialect intrinsic calls
  MPM.addPass(DXILContLgcRtOpConverterPass());

  // Add the generic continuations pipeline
  addContinuationPasses(MPM);

  // Remove dead instructions using the continuation token, which the translator
  // can't translate
  MPM.addPass(createModuleToFunctionPassAdaptor(llvm::ADCEPass()));

  // Remove code after noreturn functions like continue
  MPM.addPass(createModuleToFunctionPassAdaptor(llvm::SimplifyCFGPass()));

  MPM.addPass(DXILContPostHookPass());
}

void ContHelper::addDxilGpurtLibraryPasses(ModulePassManager &MPM) {
  MPM.addPass(llvm::DXILContIntrinsicPreparePass());
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));

  // Run some light optimizations to remove code guarded by intrinsics that were
  // replaced in the prepare pass.
  FunctionPassManager FPM;
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
  FPM.addPass(InstSimplifyPass());
  FPM.addPass(SimplifyCFGPass());
  FPM.addPass(ADCEPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
}

AnalysisKey DialectContextAnalysis::Key;

DialectContextAnalysis::DialectContextAnalysis(bool NeedDialectContext) : NeedDialectContext(NeedDialectContext) {
}

DialectContextAnalysis::Result DialectContextAnalysis::run(llvm::Module &M,
                                                           llvm::ModuleAnalysisManager &AnalysisManager) {
  if (NeedDialectContext) {
    Context = llvm_dialects::DialectContext::make<lgc::ilcps::LgcIlCpsDialect, lgc::rt::LgcRtDialect,
                                                  lgc::cps::LgcCpsDialect>(M.getContext());
  }
  return DialectContextAnalysis::Result();
}

static bool stripMDCasts(MDTuple *MDTup) {
  bool Changed = false;
  for (unsigned I = 0; I < MDTup->getNumOperands(); I++) {
    auto &MDVal = MDTup->getOperand(I);
    if (auto *Val = dyn_cast_or_null<ConstantAsMetadata>(MDVal)) {
      Constant *Const = Val->getValue();
      while (auto *Expr = dyn_cast_or_null<ConstantExpr>(Const)) {
        if (Expr->getOpcode() == Instruction::BitCast) {
          Const = Expr->getOperand(0);
        } else {
          break;
        }
      }

      if (Const != Val->getValue()) {
        auto *NewMD = ConstantAsMetadata::get(Const);
        LLVM_DEBUG(dbgs() << "Replace " << *Val->getValue() << " in metadata with " << *NewMD << "\n");
        MDTup->replaceOperandWith(I, NewMD);
        Changed = true;
      }
    }
  }

  return Changed;
}

bool llvm::fixupDxilMetadata(Module &M) {
  LLVM_DEBUG(dbgs() << "Fixing DXIL metadata\n");
  bool Changed = false;
  for (auto *MDName : {"dx.typeAnnotations", "dx.entryPoints"}) {
    if (auto *MD = M.getNamedMetadata(MDName)) {
      for (auto *Annot : MD->operands()) {
        if (auto *MDTup = dyn_cast_or_null<MDTuple>(Annot))
          Changed |= stripMDCasts(MDTup);
      }
    }
  }

  for (auto &F : M.functions()) {
    if (auto *MD = F.getMetadata(ContHelper::MDContinuationName)) {
      if (auto *MDTup = dyn_cast_or_null<MDTuple>(MD))
        Changed |= stripMDCasts(MDTup);
    }

    if (F.hasMetadata(ContHelper::MDContPayloadTyName)) {
      F.setMetadata(ContHelper::MDContPayloadTyName, nullptr);
      Changed = true;
    }
  }

  return Changed;
}

Function *llvm::getContinuationStackGlobalMemBase(Module &M) {
  auto *F = M.getFunction(ContDriverFunc::GetContinuationStackGlobalMemBaseName);
  assert(F && "Could not find GetContinuationStackGlobalMemBase function");
  assert(F->arg_size() == 0 && F->getReturnType()->isIntegerTy(64));
  return F;
}

bool llvm::isCastGlobal(GlobalValue *Global, Value *V) {
  while (auto *Expr = dyn_cast_or_null<ConstantExpr>(V)) {
    if (Expr->getOpcode() == Instruction::BitCast || Expr->getOpcode() == Instruction::AddrSpaceCast) {
      V = Expr->getOperand(0);
    } else {
      break;
    }
  }
  return Global == V;
}

uint64_t llvm::getInlineHitAttrsBytes(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  auto *GetTriangleHitAttributes = M.getFunction(ContDriverFunc::GetTriangleHitAttributesName);
  assert(GetTriangleHitAttributes && "Could not find GetTriangleHitAttributes function");
  auto *InlineHitAttrsTy = GetTriangleHitAttributes->getReturnType();
  uint64_t InlineHitAttrsBytes = DL.getTypeStoreSize(InlineHitAttrsTy).getFixedValue();
  assert((InlineHitAttrsBytes % RegisterBytes) == 0 &&
         "Size of inline hit attributes must be a multiple of the register size");
  return InlineHitAttrsBytes;
}

Function *llvm::getAccelStructAddr(Module &M, Type *HandleTy) {
  auto *Name = "amd.dx.getAccelStructAddr";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *FuncTy = FunctionType::get(I64, {HandleTy}, false);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
                                         Attribute::NoUnwind, Attribute::Speculatable, Attribute::WillReturn});
  auto *Func = cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
  Func->setOnlyAccessesArgMemory();
  Func->setOnlyReadsMemory();
  return Func;
}

Function *llvm::extractFunctionOrNull(Metadata *N) {
  auto *C = mdconst::extract_or_null<Constant>(N);
  // Strip bitcasts
  while (auto *Expr = dyn_cast_or_null<ConstantExpr>(C)) {
    if (Expr->getOpcode() == Instruction::BitCast)
      C = Expr->getOperand(0);
    else
      C = nullptr;
  }
  return dyn_cast_or_null<Function>(C);
}

bool llvm::isStartFunc(Function *Func) {
  if (auto *MD = dyn_cast_or_null<MDTuple>(Func->getMetadata(ContHelper::MDContinuationName))) {
    auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
    return Func == EntryF;
  }
  return false;
}

/// Recurse into the first member of the given SystemData to find an object of
/// the wanted type.
Value *llvm::getDXILSystemData(IRBuilder<> &B, Value *SystemData, Type *SystemDataTy, Type *Ty) {
  assert(Ty->isStructTy() && "Expected a struct type for system data");
  LLVM_DEBUG(dbgs() << "Searching for system data type " << *Ty << " in " << *SystemData << " (" << *SystemDataTy
                    << ")\n");
  Type *OrigSystemDataTy = SystemDataTy;
  SmallVector<Value *> Indices;
  // Dereference pointer
  Indices.push_back(B.getInt32(0));

  while (SystemDataTy != Ty) {
    auto *StructTy = dyn_cast<StructType>(SystemDataTy);
    if (!StructTy) {
      LLVM_DEBUG(dbgs() << "System data struct: "; SystemDataTy->dump());
      LLVM_DEBUG(dbgs() << "Wanted struct type: "; Ty->dump());
      report_fatal_error("Invalid system data struct: Did not contain the needed struct type");
    }
    SystemDataTy = StructTy->getElementType(0);
    Indices.push_back(B.getInt32(0));
  }
  if (Indices.size() == 1)
    return SystemData;
  return B.CreateInBoundsGEP(OrigSystemDataTy, SystemData, Indices);
}

CallInst *llvm::replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy, Value *SystemData,
                                     lgc::rt::RayTracingShaderStage Kind, CallInst *Call, Module *GpurtLibrary,
                                     CompilerUtils::CrossModuleInliner &Inliner) {
  B.SetInsertPoint(Call);

  auto IntrImplEntry = findIntrImplEntryByIntrinsicCall(Call);
  if (IntrImplEntry == std::nullopt)
    return nullptr;

  std::string Name = ("_cont_" + IntrImplEntry->Name).str();
  auto *IntrImpl = GpurtLibrary->getFunction(Name);
  if (!IntrImpl)
    report_fatal_error(Twine("Intrinsic implementation '") + Name + "' not found");

  SmallVector<Value *> Arguments;
  // Add the right system data type
  LLVM_DEBUG(dbgs() << "Getting system data for " << Name << "\n");
  Arguments.push_back(getDXILSystemData(B, SystemData, SystemDataTy, getFuncArgPtrElementType(IntrImpl, 0)));

  // For hit data accessors, get the hit data struct
  if (IntrImplEntry->AccessesHitData) {
    Function *GetHitData;
    if (Kind == lgc::rt::RayTracingShaderStage::AnyHit || Kind == lgc::rt::RayTracingShaderStage::Intersection) {
      auto *GetCandidateState = GpurtLibrary->getFunction(ContDriverFunc::GetCandidateStateName);
      assert(GetCandidateState && "Could not find GetCandidateState function");
      assert(GetCandidateState->getReturnType()->isStructTy() &&
             GetCandidateState->arg_size() == 1
             // Traversal data
             && GetCandidateState->getFunctionType()->getParamType(0)->isPointerTy());
      GetHitData = GetCandidateState;
    } else {
      auto *GetCommittedState = GpurtLibrary->getFunction(ContDriverFunc::GetCommittedStateName);
      assert(GetCommittedState && "Could not find GetCommittedState function");
      assert(GetCommittedState->getReturnType()->isStructTy() &&
             GetCommittedState->arg_size() == 1
             // Traversal data
             && GetCommittedState->getFunctionType()->getParamType(0)->isPointerTy());
      GetHitData = GetCommittedState;
    }
    // The intrinsic expects a pointer, so create an alloca
    auto IP = B.saveIP();
    B.SetInsertPoint(&*Call->getFunction()->begin()->begin());
    auto *HitDataAlloca = B.CreateAlloca(GetHitData->getReturnType());
    B.restoreIP(IP);
    auto *HitData =
        Inliner
            .inlineCall(B, GetHitData,
                        {getDXILSystemData(B, SystemData, SystemDataTy, getFuncArgPtrElementType(GetHitData, 0))})
            .returnValue;
    B.CreateStore(HitData, HitDataAlloca);
    Arguments.push_back(HitDataAlloca);
  }

  // Skip the intrinsic id argument, the system data argument and the hit data
  // argument
  auto *IntrType = IntrImpl->getFunctionType();
  for (unsigned CallI = 0, ImplI = IntrImplEntry->AccessesHitData ? 2 : 1, ImplE = IntrType->getNumParams();
       ImplI < ImplE; CallI++, ImplI++) {
    Value *Arg = Call->getArgOperand(CallI);
    Type *ArgType = Arg->getType();
    Type *NewType = IntrType->getParamType(ImplI);
    if (ArgType == NewType) {
      Arguments.push_back(Arg);
    } else if (NewType->isIntegerTy() && ArgType->isIntegerTy()) {
      // zext int arguments if necessary
      Arguments.push_back(B.CreateZExt(Arg, NewType));
    } else {
      std::string From;
      std::string To;
      raw_string_ostream FromStream(From);
      raw_string_ostream ToStream(To);
      ArgType->print(FromStream, true);
      NewType->print(ToStream, true);
      report_fatal_error(Twine("Can't convert ") + From + " to " + To + " for intrinsic '" + IntrImplEntry->Name + "'");
    }
  }

  auto *NewCall = B.CreateCall(IntrImpl, Arguments);
  Value *Replacement = NewCall;
  if (isa<lgc::rt::TriangleVertexPositionsOp>(Call)) {
    // Special handling for TriangleVertexPositionsOp
    // GPURT returns { <3 x float>, <3 x float>, <3 x float> }, but shader
    // requires [3 x <3 x float>].
    Replacement = PoisonValue::get(Call->getType());
    for (unsigned i = 0; i < 3; i++) {
      Replacement = B.CreateInsertValue(Replacement, B.CreateExtractValue(NewCall, i), i);
    }
  }

  LLVM_DEBUG(dbgs() << "Replacing " << *Call << " by " << *NewCall << "\n");
  if (!Call->getType()->isVoidTy())
    Call->replaceAllUsesWith(Replacement);
  Inliner.inlineCall(*NewCall);
  B.SetInsertPoint(&*B.GetInsertPoint());
  Call->eraseFromParent();
  return NewCall;
}

/// Transform enqueue intrinsics to continuation intrinsics
static bool replaceEnqueueIntrinsic(Function &F) {
  bool Changed = false;
  StringRef FuncName = F.getName();
  bool IsEnqueueCall = FuncName.contains("EnqueueCall");
  bool IsWaitEnqueue = FuncName.contains("WaitEnqueue");
  llvm_dialects::Builder B{F.getContext()};

  auto CreateContinue = [&B](const CallInst &CInst, SmallVectorImpl<Value *> &TailArgs,
                             std::optional<Value *> ReturnAddr) -> CallInst * {
    Value *ShaderAddr = CInst.getArgOperand(0);
    TailArgs.append(CInst.arg_begin() + 2, CInst.arg_end());
    return B.create<lgc::ilcps::ContinueOp>(ShaderAddr, PoisonValue::get(B.getInt32Ty()),
                                            ReturnAddr.value_or(CInst.getArgOperand(1)), TailArgs);
  };

  auto CreateWaitContinue = [&B](const CallInst &CInst, SmallVectorImpl<Value *> &TailArgs,
                                 std::optional<Value *> ReturnAddr) -> CallInst * {
    Value *ShaderAddr = CInst.getArgOperand(0);
    TailArgs.append(CInst.arg_begin() + 3, CInst.arg_end());
    Value *WaitMask = CInst.getArgOperand(1);
    return B.create<lgc::ilcps::WaitContinueOp>(ShaderAddr, WaitMask, PoisonValue::get(B.getInt32Ty()),
                                                ReturnAddr.value_or(CInst.getArgOperand(2)), TailArgs);
  };

  llvm::forEachCall(F, [&](CallInst &CInst) {
    B.SetInsertPoint(&CInst);
    SmallVector<Value *, 2> TailArgs;
    CallInst *NewCall = nullptr;
    if (IsEnqueueCall) {
      // Add the current function as return address to the call.
      // Used when Traversal calls AnyHit or Intersection.
      auto *RetAddr = B.create<lgc::cps::AsContinuationReferenceOp>(B.getInt64Ty(), CInst.getFunction());
      if (IsWaitEnqueue) {
        // Handle WaitEnqueueCall.
        NewCall = CreateWaitContinue(CInst, TailArgs, RetAddr);
      } else {
        // Handle EnqueueCall.
        NewCall = CreateContinue(CInst, TailArgs, RetAddr);
      }

    } else if (IsWaitEnqueue) {
      // Handle WaitEnqueue.
      NewCall = CreateWaitContinue(CInst, TailArgs, std::nullopt);
    } else {
      // Handle Enqueue.
      NewCall = CreateContinue(CInst, TailArgs, std::nullopt);
    }

    // NOTE: Inlining ExitRayGen in LowerRaytracingPipeline can cause continue
    // ops whose name is suffixed .cloned.*, which don't get picked up by the
    // direct name comparison we use when checking for existence of payload
    // metadata in DXILContPostProcess. With the new dialect ops, these get
    // picked up, so they need to have outgoing register count.
    if (NewCall->getFunction()->getName() == ContDriverFunc::ExitRayGenName)
      ContHelper::OutgoingRegisterCount::setValue(NewCall, 0);

    CompilerUtils::createUnreachable(B);
    Changed = true;
  });

  return Changed;
}

static void handleContinuationStackIsGlobal(Function &Func, ContStackAddrspace StackAddrspace) {
  assert(Func.arg_empty()
         // bool
         && Func.getFunctionType()->getReturnType()->isIntegerTy(1));

  auto *IsGlobal = ConstantInt::getBool(Func.getContext(), StackAddrspace == ContStackAddrspace::Global);

  llvm::replaceCallsToFunction(Func, *IsGlobal);
}

static void handleContinuationsGetFlags(Function &Func, uint32_t Flags) {
  assert(Func.arg_empty()
         // i32
         && Func.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto *FlagsConst = ConstantInt::get(IntegerType::get(Func.getContext(), 32), Flags);

  llvm::replaceCallsToFunction(Func, *FlagsConst);
}

static void handleGetRtip(Function &Func, uint32_t RtipLevel) {
  assert(Func.arg_empty()
         // i32
         && Func.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto *RtipConst = ConstantInt::get(IntegerType::get(Func.getContext(), 32), RtipLevel);
  for (auto &Use : make_early_inc_range(Func.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        CInst->replaceAllUsesWith(RtipConst);
        CInst->eraseFromParent();
      }
    }
  }
}

static void handleGetUninitialized(Function &Func) {
  auto *ArgTy = Func.getReturnType();
  auto *Poison = PoisonValue::get(ArgTy);
  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    IRBuilder<> B(&CInst);
    // Create a frozen poison value so poison doesn't propagate into
    // dependent values, e.g. when bitpacking the uninitialized value into
    // a bitfield that should not be invalidated.
    Value *Freeze = B.CreateFreeze(Poison);
    CInst.replaceAllUsesWith(Freeze);
    CInst.eraseFromParent();
  });
}

void ContHelper::handleGetSetting(Function &F, ArrayRef<ContSetting> Settings) {
  auto *Ty = dyn_cast<IntegerType>(F.getReturnType());
  if (!Ty)
    report_fatal_error(Twine("Only integer settings are supported but '") + F.getName() +
                       "' does not return an integer");
  auto Name = F.getName();
  bool Consumed = Name.consume_front("_AmdGetSetting_");
  if (!Consumed)
    report_fatal_error(Twine("Setting intrinsic needs to start with "
                             "'_AmdGetSetting_' but is called '") +
                       Name + "'");

  uint64_t NameVal;
  bool Failed = Name.getAsInteger(10, NameVal);
  if (Failed) {
    report_fatal_error(Twine("Failed to parse _AmdGetSetting_ suffix as int: ") + Name);
  }

  uint64_t Value = 0;
  bool Found = false;
  for (auto &Setting : Settings) {
    if (Setting.NameHash == NameVal) {
      Value = Setting.Value;
      Found = true;
      break;
    }
  }
  if (!Found) {
#ifndef NDEBUG
    errs() << Twine("Warning: Setting '") + Name + "' is not defined, setting to 0\n";
#endif
  }

  auto *Val = ConstantInt::get(Ty, Value);

  replaceCallsToFunction(F, *Val);
}

void ContHelper::handleGetFuncAddr(Function &F, llvm_dialects::Builder &Builder) {
  assert(F.arg_empty()
         // returns i64 or i32
         && (F.getFunctionType()->getReturnType()->isIntegerTy(64) ||
             F.getFunctionType()->getReturnType()->isIntegerTy(32)));

  auto Name = F.getName();
  [[maybe_unused]] bool Consumed = Name.consume_front("_AmdGetFuncAddr");
  assert(Consumed);

  Function *Impl = F.getParent()->getFunction(Name);
  if (!Impl)
    report_fatal_error(Twine("Did not find function '") + Name + "' requested by _AmdGetFuncAddr");

  llvm::forEachCall(F, [&](llvm::CallInst &CInst) {
    auto *RetTy = F.getReturnType();
    Builder.SetInsertPoint(&CInst);
    Value *AsContRef = Builder.create<lgc::cps::AsContinuationReferenceOp>(RetTy, Impl);
    CInst.replaceAllUsesWith(AsContRef);
    CInst.eraseFromParent();
  });
}

void ContHelper::handleValueI32Count(Function &F, IRBuilder<> &Builder) {
  assert(F.arg_size() == 1
         // i32 count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy());

  auto *Ty = getFuncArgPtrElementType(&F, 0);
  auto *Size = Builder.getInt32(divideCeil(F.getParent()->getDataLayout().getTypeStoreSize(Ty).getFixedValue(), 4));
  llvm::replaceCallsToFunction(F, *Size);
}

void ContHelper::handleValueGetI32(Function &F, IRBuilder<> &Builder) {
  assert(F.arg_size() == 2
         // value
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  auto *I32 = Builder.getInt32Ty();

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    Value *Addr = CInst.getArgOperand(0);
    Addr = Builder.CreateGEP(I32, Addr, CInst.getArgOperand(1));
    auto *Load = Builder.CreateLoad(I32, Addr);
    CInst.replaceAllUsesWith(Load);
    CInst.eraseFromParent();
  });
}

void ContHelper::handleValueSetI32(Function &F, IRBuilder<> &Builder) {
  assert(F.arg_size() == 3 &&
         F.getFunctionType()->getReturnType()->isVoidTy()
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(2)->isIntegerTy(32));

  auto *I32 = Builder.getInt32Ty();
  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    Value *Addr = CInst.getArgOperand(0);
    Addr = Builder.CreateGEP(I32, CInst.getArgOperand(0), CInst.getArgOperand(1));
    Builder.CreateStore(CInst.getArgOperand(2), Addr);
    CInst.eraseFromParent();
  });
}

void llvm::terminateShader(IRBuilder<> &Builder, CallInst *CompleteCall) {
  Builder.SetInsertPoint(CompleteCall);

  [[maybe_unused]] Instruction *OldTerminator = CompleteCall->getParent()->getTerminator();
  Type *FuncRetTy = CompleteCall->getFunction()->getReturnType();
  // For functions returning a value, return a poison. Resume functions
  // and other shaders will simply return a void value when this helper is being
  // called from LegacyCleanupContinuations. These will be treated as
  // continuation.complete by the translator.
  ReturnInst *Ret = nullptr;
  if (FuncRetTy->isVoidTy())
    Ret = Builder.CreateRetVoid();
  else
    Ret = Builder.CreateRet(PoisonValue::get(FuncRetTy));

  assert(OldTerminator != CompleteCall && "terminateShader: Invalid terminator instruction provided!");

  // If there is some code after the call to _AmdComplete or the intended
  // lgc.ilcps.return that aborts the shader, do the following:
  // - Split everything after the completion call into a separate block
  // - Remove the newly inserted unconditional branch to the split block
  // - Remove the complete call.
  // This is intended to work for _AmdComplete appearing in conditional code
  // or the unreachable inserted by various passes before
  // LegacyCleanupContinuations.
  SplitBlock(CompleteCall->getParent(), CompleteCall);
  // Remove the branch to the split block.
  Ret->getParent()->getTerminator()->eraseFromParent();
  CompleteCall->eraseFromParent();
}

bool llvm::earlyDriverTransform(Module &M) {
  // Import StackAddrspace from metadata if set, otherwise from default
  auto StackAddrspaceMD = ContHelper::tryGetStackAddrspace(M);
  auto StackAddrspace = StackAddrspaceMD.value_or(ContHelper::DefaultStackAddrspace);

  // Import from metadata if set
  auto RtipLevel = ContHelper::Rtip::tryGetValue(&M);
  auto Flags = ContHelper::Flags::tryGetValue(&M);
  SmallVector<ContSetting> GpurtSettings;
  ContHelper::getGpurtSettings(M, GpurtSettings);

  bool Changed = false;
  // Replace Enqueue and Complete intrinsics
  for (auto &F : M) {
    auto Name = F.getName();

    if (Name.contains("Enqueue")) {
      Changed = replaceEnqueueIntrinsic(F);
    }

    if (Name.starts_with("_AmdContinuationStackIsGlobal")) {
      Changed = true;
      handleContinuationStackIsGlobal(F, StackAddrspace);
    } else if (Name.starts_with("_AmdContinuationsGetFlags")) {
      Changed = true;
      if (!Flags)
        report_fatal_error("Tried to get continuation flags but it is not "
                           "available on the module");
      handleContinuationsGetFlags(F, *Flags);
    } else if (Name.starts_with("_AmdGetRtip")) {
      Changed = true;
      if (!RtipLevel)
        report_fatal_error("Tried to get rtip level but it is not available on the module");
      handleGetRtip(F, *RtipLevel);
    } else if (Name.starts_with("_AmdGetUninitialized")) {
      Changed = true;
      handleGetUninitialized(F);
    } else if (Name.starts_with("_AmdGetSetting")) {
      Changed = true;
      ContHelper::handleGetSetting(F, GpurtSettings);
    }
  }

  return Changed;
}

uint64_t llvm::computePayloadSpillSize(uint64_t NumI32s, uint64_t NumReservedRegisters) {
  if (NumI32s <= NumReservedRegisters)
    return 0;

  uint64_t NumStackI32s = NumI32s - NumReservedRegisters;
  return NumStackI32s * RegisterBytes;
}

namespace llvm {
namespace coro {
bool defaultMaterializable(Instruction &V);
} // End namespace coro
} // End namespace llvm

bool llvm::commonMaterializable(Instruction &Inst) {
  if (coro::defaultMaterializable(Inst))
    return true;

  // Insert into constant.
  if (isa<InsertElementInst, InsertValueInst>(Inst) && isa<Constant>(Inst.getOperand(0))) {
    return true;
  }

  if (auto *Shuffle = dyn_cast<ShuffleVectorInst>(&Inst); Shuffle && Shuffle->isSingleSource())
    return true;

  return false;
}

bool llvm::LgcMaterializable(Instruction &OrigI) {
  Instruction *V = &OrigI;

  // extract instructions are rematerializable, but increases the size of the
  // continuation state, so as a heuristic only rematerialize this if the source
  // can be rematerialized as well.
  while (true) {
    Instruction *NewInst = nullptr;
    if (auto *Val = dyn_cast<ExtractElementInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getVectorOperand());
    else if (auto *Val = dyn_cast<ExtractValueInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getAggregateOperand());

    if (NewInst)
      V = NewInst;
    else
      break;
  }

  if (commonMaterializable(*V))
    return true;

  if (auto *LI = dyn_cast<LoadInst>(V)) {
    // load from constant address space
    if (LI->getPointerAddressSpace() == 4)
      return true;
  }

  if (auto *CInst = dyn_cast<CallInst>(V)) {
    if (auto *CalledFunc = CInst->getCalledFunction()) {
      // Before rematerialization happens, lgc.rt dialect operations that cannot
      // be rematerialized are replaced by their implementation, so that the
      // necessary values can be put into the coroutine frame. Therefore, we
      // can assume all left-over intrinsics can be rematerialized.
      if (ContHelper::isRematerializableLgcRtOp(*CInst))
        return true;

      if (auto *Intrinsic = dyn_cast<IntrinsicInst>(CInst)) {
        switch (Intrinsic->getIntrinsicID()) {
        // Note: s_getpc will return a different value if rematerialized into a
        // different place, but assuming we only care about the high 32bit for
        // all the use cases we have now, it should be ok to do so.
        case Intrinsic::amdgcn_s_getpc:
          return true;
        default:
          break;
        }
      }

      auto CalledName = CalledFunc->getName();
      // FIXME: switch to dialectOp check.
      if (CalledName.starts_with("lgc.user.data") || CalledName.starts_with("lgc.shader.input") ||
          CalledName.starts_with("lgc.create.get.desc.ptr") || CalledName.starts_with("lgc.load.buffer.desc") ||
          CalledName.starts_with("lgc.load.user.data"))
        return true;
    }
  }

  return false;
}

std::optional<CallInst *> llvm::findDominatedContinueCall(CallInst *GetResPointAddr) {
  SmallDenseSet<BasicBlock *> Visited;
  SmallDenseSet<BasicBlock *> UnknownPreds;
  SmallVector<BasicBlock *> WorkList;
  CallInst *Candidate = nullptr;
  Visited.insert(GetResPointAddr->getParent());
  WorkList.push_back(GetResPointAddr->getParent());

  while (!WorkList.empty()) {
    auto *BB = WorkList.pop_back_val();
    // Check predecessors
    if (BB != GetResPointAddr->getParent()) {
      for (auto *Pred : predecessors(BB)) {
        if (!Visited.contains(Pred))
          UnknownPreds.insert(Pred);
      }
    }

    auto *Terminator = BB->getTerminator();
    if (isa_and_nonnull<UnreachableInst>(Terminator)) {
      auto Before = --Terminator->getIterator();
      if (auto *ContinueCall = dyn_cast<CallInst>(Before)) {
        if (Candidate != nullptr) {
          LLVM_DEBUG(dbgs() << "Found multiple continue candidates after a "
                               "GetResumePointAddr:\n";
                     Candidate->dump(); ContinueCall->dump());
          return {};
        }
        Candidate = ContinueCall;
      } else {
        LLVM_DEBUG(dbgs() << "The BB must end in a (continue) call after a "
                             "GetResumePointAddr, but "
                          << BB->getName() << " doesn't");
        return {};
      }
    }

    for (auto *Succ : successors(BB)) {
      if (Visited.contains(Succ))
        continue;
      Visited.insert(Succ);
      UnknownPreds.erase(Succ);
      WorkList.push_back(Succ);
    }
  }

  if (Candidate == nullptr) {
    LLVM_DEBUG(dbgs() << "Did not find a continue call after a GetResumePointAddr\n");
    return {};
  }

  if (!UnknownPreds.empty()) {
    LLVM_DEBUG(dbgs() << "Found more than one predecessor for the continue "
                         "call after a GetResumePointAddr:\n";
               for (auto *Pred
                    : UnknownPreds) Pred->dump(););
    return {};
  }

  return Candidate;
}

namespace llvm {
void addLgcContinuationTransform(ModulePassManager &MPM) {
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));

  MPM.addPass(LowerAwaitPass());

  MPM.addPass(CoroEarlyPass());
  CGSCCPassManager CGPM;
  CGPM.addPass(LgcCoroSplitPass());
  MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(std::move(CGPM)));
  MPM.addPass(createModuleToFunctionPassAdaptor(CoroElidePass()));
  MPM.addPass(CoroCleanupPass());

  MPM.addPass(CleanupContinuationsPass());

#ifndef NDEBUG
  MPM.addPass(ContinuationsLintPass());
#endif

  MPM.addPass(createModuleToFunctionPassAdaptor(LowerSwitchPass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(FixIrreduciblePass()));
}
} // End namespace llvm
