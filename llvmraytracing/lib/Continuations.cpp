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

//===- Continuations.cpp - Continuations utilities ------------------------===//
//
// This file defines implementations for helper functions for continuation
// passes.
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "ContStateBuilder.h"
#include "RematSupport.h"
#include "compilerutils/ArgPromotion.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/DxilToLlvm.h"
#include "compilerutils/TypesMetadata.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "llvmraytracing/SpecializeDriverShaders.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntervalTree.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/Attributes.h"
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
#include "llvm/Transforms/Scalar/Scalarizer.h"
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

bool llvm::replaceCallsToFunction(Function &F, Value &Replacement) {
  bool Changed = false;

  llvm::forEachCall(F, [&](CallInst &CInst) {
    // Basic sanity check. We should also check for dominance.
    assert((!isa<Instruction>(&Replacement) || cast<Instruction>(&Replacement)->getFunction() == CInst.getFunction()) &&
           "llvm::replaceCallsToFunction: Replacement should "
           "reside in the same function as CallInst to replace!");
    CInst.replaceAllUsesWith(&Replacement);
    CInst.eraseFromParent();

    Changed = true;
  });

  return Changed;
}

void llvm::moveFunctionBody(Function &OldFunc, Function &NewFunc) {
  while (!OldFunc.empty()) {
    BasicBlock *BB = &OldFunc.front();
    BB->removeFromParent();
    BB->insertInto(&NewFunc);
  }
}

std::optional<llvm::GpuRtIntrinsicEntry> llvm::findIntrImplEntryByIntrinsicCall(CallInst *Call) {
  if (!lgc::rt::LgcRtDialect::isDialectOp(*Call->getCalledFunction()))
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
      if (!OnlyIntrinsics || (lgc::rt::LgcRtDialect::isDialectOp(F) || F.getName().starts_with("dx.op."))) {
        F.eraseFromParent();
        DidChange = true;
      }
    }
  }

  return DidChange;
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

  MPM.addPass(SpecializeDriverShadersPass());

  MPM.addPass(LowerAwaitPass());

  MPM.addPass(CoroEarlyPass());
  MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(DXILCoroSplitPass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(CoroElidePass()));
  MPM.addPass(CoroCleanupPass());

  MPM.addPass(CleanupContinuationsPass());
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

  // Fixup DXIL vs LLVM incompatibilities. This needs to run first.
  // If we add more LLVM processing separate from continuation passes,
  // we potentially should do it earlier as part of the module loading.
  MPM.addPass(CompilerUtils::DxilToLlvmPass());

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
  MPM.addPass(CompilerUtils::DxilToLlvmPass());

  MPM.addPass(llvm::DXILContPrepareGpurtLibraryPass());
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));

  // Run some light optimizations to remove code guarded by intrinsics that were
  // replaced in the prepare pass.
  FunctionPassManager FPM;
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
  FPM.addPass(InstSimplifyPass());
  FPM.addPass(SimplifyCFGPass());
  // Intentionally do another round of InstSimplify+SimplifyCFG to ensure traits in Gpurt are fully optimized out
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

Function *llvm::getStartFunc(Function *Func) {
  if (auto *MD = dyn_cast_or_null<MDTuple>(Func->getMetadata(ContHelper::MDContinuationName))) {
    Function *StartFunc = extractFunctionOrNull(MD->getOperand(0));
    if (StartFunc != nullptr)
      return StartFunc;
  }
  return Func;
}

bool llvm::isStartFunc(Function *Func) {
  return Func == getStartFunc(Func);
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

Value *llvm::replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy, Value *SystemData,
                                  lgc::rt::RayTracingShaderStage Kind, CallInst *Call, Module *GpurtLibrary,
                                  CompilerUtils::CrossModuleInliner &Inliner, bool KeepBuilderPos) {
  if (!KeepBuilderPos)
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

  // Tolerate Replacement returning a single-element struct containing a value of the right type.
  // That happens when the called function is _cont_ObjectToWorld4x3 (and possibly others) from LLPCFE.
  if (!Call->getType()->isVoidTy() && Call->getType() != Replacement->getType()) {
    assert(cast<StructType>(Replacement->getType())->getNumElements() == 1);
    Replacement = B.CreateExtractValue(Replacement, 0);
  }

  LLVM_DEBUG(dbgs() << "Replacing " << *Call << " by " << *NewCall << "\n");
  // Add a fake-use so we can get the replaced value afterwards
  FreezeInst *FakeUse = nullptr;
  if (!Call->getType()->isVoidTy()) {
    Call->replaceAllUsesWith(Replacement);
    FakeUse = cast<FreezeInst>(B.CreateFreeze(Replacement));
  }
  Inliner.inlineCall(*NewCall);
  auto *OldInsertPt = &*B.GetInsertPoint();
  // If insert point happens to be `Call`, move it to the next instruction
  B.SetInsertPoint(OldInsertPt == Call ? Call->getNextNode() : OldInsertPt);

  Call->eraseFromParent();
  // Inlined, so original replacement is now invalid
  Replacement = nullptr;

  if (FakeUse) {
    Replacement = FakeUse->getOperand(0);
    FakeUse->eraseFromParent();
  }
  return Replacement;
}

/// Transform enqueue intrinsics to continuation intrinsics
static bool replaceEnqueueIntrinsic(Function &F) {
  bool Changed = false;
  StringRef FuncName = F.getName();
  bool IsWaitEnqueue = FuncName.contains("WaitEnqueue");
  llvm_dialects::Builder B{F.getContext()};

  llvm::forEachCall(F, [&](CallInst &CInst) {
    B.SetInsertPoint(&CInst);
    CallInst *NewCall = nullptr;
    Value *WaitMask = nullptr;
    Value *ShaderRecIdx = nullptr;
    Value *RetAddr = nullptr;
    if (IsWaitEnqueue) {
      // Handle WaitEnqueue.
      WaitMask = CInst.getArgOperand(1);
      ShaderRecIdx = CInst.getArgOperand(2);
      RetAddr = CInst.getArgOperand(3);
    } else {
      ShaderRecIdx = CInst.getArgOperand(1);
      RetAddr = CInst.getArgOperand(2);
    }

    SmallVector<Value *> TailArgs;
    const uint32_t TailArgStartIdx = WaitMask ? 4 : 3;
    TailArgs.append(CInst.arg_begin() + TailArgStartIdx, CInst.arg_end());

    // For DX, these arguments are unused right now and are just here to fulfill the `JumpOp`s requirements as being
    // defined in the LgcCpsDialect.
    const uint32_t DummyLevelsArg = -1;
    Value *DummyCsp = PoisonValue::get(B.getInt32Ty());
    NewCall =
        B.create<lgc::cps::JumpOp>(CInst.getArgOperand(0), DummyLevelsArg, DummyCsp, ShaderRecIdx, RetAddr, TailArgs);

    if (WaitMask) {
      // The only supported wait mask is a constant -1. We don't enforce having a constant here because the SPIR-V
      // build of GPURT isn't optimized.
      assert(!isa<ConstantInt>(WaitMask) || cast<ConstantInt>(WaitMask)->getSExtValue() == -1);
      ContHelper::setWaitMask(*NewCall);
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

/// Remove wait mask from WaitAwait intrinsic calls and set waitmask metadata if PreserveWaitMasks is set to true
static bool replaceAwaitIntrinsic(Function &F, bool PreserveWaitMasks = true) {
  StringRef FuncName = F.getName();

  if (FuncName.contains("AmdAwait"))
    return false;

  if (!FuncName.contains("AmdWaitAwait"))
    report_fatal_error("replaceAwaitIntrinsic: Unexpected await call!");

  IRBuilder<> B{F.getContext()};
  SmallVector<CallInst *> ErasableAwaits;

  llvm::forEachCall(F, [&](CallInst &CInst) {
    [[maybe_unused]] ConstantInt *WaitMask = cast<ConstantInt>(CInst.getArgOperand(1));
    assert(WaitMask->getSExtValue() == -1);

    SmallVector<Value *> NewArgs{CInst.args()};
    NewArgs.erase(NewArgs.begin() + 1);

    B.SetInsertPoint(&CInst);
    auto *NewCall = CompilerUtils::createNamedCall(B, "_AmdAwait", CInst.getType(), NewArgs, {});
    CInst.replaceAllUsesWith(NewCall);
    if (PreserveWaitMasks)
      ContHelper::setWaitMask(*NewCall);

    ErasableAwaits.push_back(&CInst);
  });

  // Cleanup old await calls
  for (auto *OldAwait : ErasableAwaits)
    OldAwait->eraseFromParent();

  return !ErasableAwaits.empty();
}

static void handleContinuationStackIsGlobal(Function &Func, ContStackAddrspace StackAddrspace) {
  assert(Func.arg_empty()
         // bool
         && Func.getFunctionType()->getReturnType()->isIntegerTy(1));

  auto *IsGlobal = ConstantInt::getBool(Func.getContext(), StackAddrspace == ContStackAddrspace::Global ||
                                                               StackAddrspace == ContStackAddrspace::GlobalLLPC);

  llvm::replaceCallsToFunction(Func, *IsGlobal);
}

static void handleGetRtip(Function &Func, uint32_t RtipLevel) {
  assert(Func.arg_empty()
         // i32
         && Func.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto *RtipConst = ConstantInt::get(IntegerType::get(Func.getContext(), 32), RtipLevel);
  llvm::replaceCallsToFunction(Func, *RtipConst);
}

static void handleGetUninitialized(Function &Func) {
  auto *ArgTy = Func.getReturnType();
  auto *Poison = PoisonValue::get(ArgTy);
  IRBuilder<> B{Func.getContext()};
  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    B.SetInsertPoint(&CInst);
    // Create a frozen poison value so poison doesn't propagate into
    // dependent values, e.g. when bitpacking the uninitialized value into
    // a bitfield that should not be invalidated.
    Value *Freeze = B.CreateFreeze(Poison);
    CInst.replaceAllUsesWith(Freeze);
    CInst.eraseFromParent();
  });
}

void ContHelper::handleComplete(Function &Func) {
  llvm::forEachCall(Func, [&](llvm::CallInst &CInst) {
    llvm_dialects::Builder B{&CInst};
    B.create<lgc::cps::CompleteOp>();
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
         // returns i32
         && F.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto Name = F.getName();
  [[maybe_unused]] bool Consumed = Name.consume_front("_AmdGetFuncAddr");
  assert(Consumed);

  Function *Impl = F.getParent()->getFunction(Name);
  if (!Impl)
    report_fatal_error(Twine("Did not find function '") + Name + "' requested by _AmdGetFuncAddr");

  llvm::forEachCall(F, [&](llvm::CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    Value *AsContRef = Builder.create<lgc::cps::AsContinuationReferenceOp>(Impl);
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

Function *llvm::tryGpurtPointerArgPromotion(Function *Func) {
  StringRef FuncName = Func->getName();

  if (!Func->hasMetadata(TypedFuncTy::MDTypesName) && !Func->arg_empty())
    return nullptr;

  SmallBitVector PromotionMask(Func->arg_size());
  for (auto [Index, Arg] : llvm::enumerate(Func->args())) {
    TypedArgTy ArgTy = TypedArgTy::get(&Arg);
    if (!ArgTy.isPointerTy())
      continue;

    // Change the pointer type to its value type for non-struct types.
    // _Amd*Await use value types for all arguments.
    // For _cont_SetTriangleHitAttributes, we always use its value type for hitAttributes argument.
    // Include Traversal, since we want the system data to be of struct type.
    if (!isa<StructType>(ArgTy.getPointerElementType()) || FuncName.contains("Enqueue") || FuncName.contains("Await") ||
        FuncName == ContDriverFunc::TraversalName ||
        (FuncName == ContDriverFunc::SetTriangleHitAttributesName && Index == 1))
      PromotionMask.set(Index);
  }

  // promotePointerArguments returns the input if no argument was promoted.
  auto *NewFunc = CompilerUtils::promotePointerArguments(Func, PromotionMask);

  // This function is provided by the compiler to GPURT. It will be substituted by LowerRaytracingPipeline.
  // NOTE: GPURT now preserves all function names started with "_Amd", but some of them are not intrinsics, e.g.,
  // "_AmdSystemData.IsTraversal", which are methods of system data structs. Skip those to let them be inlined
  // automatically.
  if (NewFunc->getName().contains("_Amd") && !NewFunc->getName().contains(".")) {
    // Metadata can be cleared by the call to deleteBody, so ensure the prototypes still have it, since we
    // later rely on it.
    auto *ClonedMD = NewFunc->getMetadata(TypedFuncTy::MDTypesName);
    NewFunc->deleteBody();
    NewFunc->setMetadata(TypedFuncTy::MDTypesName, ClonedMD);
  }

  if (PromotionMask.any())
    return NewFunc;

  return nullptr;
}

bool llvm::earlyGpurtTransform(Module &M, SmallVector<Function *> &PromotableFunctions, bool PreserveWaitMasks) {
  // Import StackAddrspace from metadata if set, otherwise from default
  auto StackAddrspaceMD = ContHelper::tryGetStackAddrspace(M);
  auto StackAddrspace = StackAddrspaceMD.value_or(ContHelper::DefaultStackAddrspace);

  // Import from metadata if set
  auto RtipLevel = ContHelper::Rtip::tryGetValue(&M);
  SmallVector<ContSetting> GpurtSettings;
  ContHelper::getGpurtSettings(M, GpurtSettings);

  bool Changed = false;

  // Try the argument promotion
  for (Function *PromotableFunc : PromotableFunctions) {
    Function *PromotedFunc = tryGpurtPointerArgPromotion(PromotableFunc);
    if (PromotedFunc)
      Changed = true;
  }

  // Replace Enqueue and Complete intrinsics
  for (auto &F : M) {
    auto Name = F.getName();

    if (Name.contains("Enqueue")) {
      Changed = replaceEnqueueIntrinsic(F);
    } else if (Name.contains("Await")) {
      Changed = replaceAwaitIntrinsic(F, PreserveWaitMasks);
    }

    if (Name.starts_with("_AmdContinuationStackIsGlobal")) {
      Changed = true;
      handleContinuationStackIsGlobal(F, StackAddrspace);
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
    } else if (Name.starts_with("_AmdComplete")) {
      Changed = true;
      ContHelper::handleComplete(F);
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

DXILCoroSplitPass::DXILCoroSplitPass()
    : CoroSplitPass(std::function<bool(Instruction &)>(&rematsupport::DXILMaterializable),
                    {[](Function &F, coro::Shape &S) {
                      return std::make_unique<llvmraytracing::ContStateBuilder>(F, S, rematsupport::DXILMaterializable);
                    }},
                    /*OptimizeFrame*/ true) {
}

LgcCoroSplitPass::LgcCoroSplitPass()
    : CoroSplitPass(std::function<bool(Instruction &)>(&rematsupport::LgcMaterializable),
                    {[](Function &F, coro::Shape &S) {
                      return std::make_unique<llvmraytracing::ContStateBuilder>(F, S, rematsupport::LgcMaterializable);
                    }},
                    /*OptimizeFrame*/ true) {
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

  // Scalarizer pass could break down system data structure (and possibly other data) which would help to reduce size of
  // continuations state.
  ScalarizerPassOptions scalarizerOptions;
  scalarizerOptions.ScalarizeMinBits = 32;
  MPM.addPass(createModuleToFunctionPassAdaptor(ScalarizerPass(scalarizerOptions)));

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
  MPM.addPass(ContinuationsStatsReportPass());

  MPM.addPass(createModuleToFunctionPassAdaptor(LowerSwitchPass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(FixIrreduciblePass()));
}
} // End namespace llvm
