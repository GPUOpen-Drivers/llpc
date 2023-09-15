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

//===- DXILCont.cpp - Insert await calls and prepare DXIL -----------------===//
//
// This file serves as a caller for the LowerRaytracingPipelineImpl.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "continuations/ContinuationsUtil.h"
#include "continuations/LowerRaytracingPipeline.h"
#include "lgcrt/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Coroutines/CoroCleanup.h"
#include "llvm/Transforms/Coroutines/CoroEarly.h"
#include "llvm/Transforms/Coroutines/CoroElide.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/FixIrreducible.h"

#define DEBUG_TYPE "lower-raytracing-pipeline"

using namespace llvm;

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

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &Str,
                                    DXILShaderKind ShaderKind) {
  Str << ::toString(ShaderKind);
  return Str;
}

void DXILContHelper::RegisterPasses(PassBuilder &PB, bool NeedDialectContext) {
#define HANDLE_PASS(NAME, CREATE_PASS)                                         \
  if (innerPipeline.empty() && name == NAME) {                                 \
    passMgr.addPass(CREATE_PASS);                                              \
    return true;                                                               \
  }

#define HANDLE_ANALYSIS(NAME, CREATE_PASS, IRUNIT)                             \
  if (innerPipeline.empty() && name == "require<" NAME ">") {                  \
    passMgr.addPass(                                                           \
        RequireAnalysisPass<std::remove_reference_t<decltype(CREATE_PASS)>,    \
                            IRUNIT>());                                        \
    return true;                                                               \
  }                                                                            \
  if (innerPipeline.empty() && name == "invalidate<" NAME ">") {               \
    passMgr.addPass(InvalidateAnalysisPass<                                    \
                    std::remove_reference_t<decltype(CREATE_PASS)>>());        \
    return true;                                                               \
  }

  PB.registerPipelineParsingCallback(
      [](StringRef name, ModulePassManager &passMgr,
         ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_MODULE_PASS HANDLE_PASS
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS)                                \
  HANDLE_ANALYSIS(NAME, CREATE_PASS, Module)
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &PassMgr,
         ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_FUNCTION_PASS HANDLE_PASS
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef Name, LoopPassManager &PassMgr,
         ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_LOOP_PASS HANDLE_PASS
#include "PassRegistry.inc"

        return false;
      });

  PB.registerPipelineParsingCallback(
      [](StringRef name, ModulePassManager &passMgr,
         ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef Params;
        (void)Params;
#define CONT_CGSCC_PASS(NAME, CREATE_PASS)                                     \
  if (innerPipeline.empty() && name == NAME) {                                 \
    passMgr.addPass(createModuleToPostOrderCGSCCPassAdaptor(CREATE_PASS));     \
    return true;                                                               \
  }
#include "PassRegistry.inc"
        return false;
      });

#undef HANDLE_ANALYSIS
#undef HANDLE_PASS

  PB.registerAnalysisRegistrationCallback(
      [=](ModuleAnalysisManager &AnalysisManager) {
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS)                                \
  AnalysisManager.registerPass([&] { return CREATE_PASS; });
#include "PassRegistry.inc"
      });

  auto *PIC = PB.getPassInstrumentationCallbacks();
  if (PIC) {
#define CONT_PASS(NAME, CREATE_PASS)                                           \
  PIC->addClassToPassName(decltype(CREATE_PASS)::name(), NAME);
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS)                                \
  PIC->addClassToPassName(decltype(CREATE_PASS)::name(), NAME);
#include "PassRegistry.inc"
  }
}

void DXILContHelper::addContinuationPasses(ModulePassManager &MPM) {
  MPM.addPass(LowerRaytracingPipelinePass());

  // Inline TraceRay and similar intrinsic implementations
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));

  // Splits basic blocks after the systemDataRestored marker and removes already
  // inlined intrinsic implementations
  MPM.addPass(DXILContPreCoroutinePass());

  // Convert the system data struct to a value, so it isn't stored in the
  // continuation state
  MPM.addPass(createModuleToFunctionPassAdaptor(
      SROAPass(llvm::SROAOptions::ModifyCFG)));
  MPM.addPass(LowerAwaitPass());

  MPM.addPass(CoroEarlyPass());
  CGSCCPassManager CGPM;
  CGPM.addPass(DXILCoroSplitPass());
  MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(std::move(CGPM)));
  MPM.addPass(createModuleToFunctionPassAdaptor(CoroElidePass()));
  MPM.addPass(CoroCleanupPass());

  MPM.addPass(CleanupContinuationsPass());
  MPM.addPass(RegisterBufferPass());
  MPM.addPass(SaveContinuationStatePass());
  MPM.addPass(DXILContPostProcessPass());

  MPM.addPass(RemoveTypesMetadataPass());

  // Splitting functions as part of LLVM's coroutine transformation can lead
  // to irreducible resume functions in some cases. Use the FixIrreduciblePass
  // to resolve the irreducibility with a dynamic dispatch block. In the future
  // we might want to use node splitting instead for better perf, or a
  // combination of the two. Note: Even if the control flow is reducible, this
  // pass can still change the module in its preprocessing, lowering switches to
  // chained ifs.
  MPM.addPass(createModuleToFunctionPassAdaptor(FixIrreduciblePass()));

  // Inline remaining intrinsic implementations
  MPM.addPass(AlwaysInlinerPass(/*InsertLifetimeIntrinsics=*/false));
}

void DXILContHelper::addDxilContinuationPasses(ModulePassManager &MPM) {
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

AnalysisKey DialectContextAnalysis::Key;

DialectContextAnalysis::DialectContextAnalysis(bool NeedDialectContext)
    : NeedDialectContext(NeedDialectContext) {}

DialectContextAnalysis::Result
DialectContextAnalysis::run(llvm::Module &M,
                            llvm::ModuleAnalysisManager &AnalysisManager) {
  if (NeedDialectContext) {
    Context =
        llvm_dialects::DialectContext::make<continuations::ContinuationsDialect,
                                            lgc::rt::LgcRtDialect>(
            M.getContext());
  }
  return DialectContextAnalysis::Result();
}

llvm::PreservedAnalyses
LowerRaytracingPipelinePass::run(llvm::Module &M,
                                 llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass lower-raytracing-pipeline\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  LowerRaytracingPipelinePassImpl Impl(M);
  bool Changed = Impl.run();

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

std::pair<LoadInst *, Value *> llvm::moveContinuationStackOffset(IRBuilder<> &B,
                                                                 int32_t I) {
  // %cont.frame.mem = load i32, i32* %csp
  // %newcsp = add i32 %cont.frame.mem, I
  // store i32 %newcsp, i32 %csp

  auto *CspType = getContinuationStackOffsetType(B.getContext());
  auto *Csp = B.CreateCall(
      getContinuationStackOffset(*B.GetInsertPoint()->getModule()));
  auto *OldCsp = B.CreateLoad(CspType, Csp);
  auto *NewCsp = B.CreateAdd(OldCsp, B.getInt32(I));
  B.CreateStore(NewCsp, Csp);

  return std::make_pair(OldCsp, NewCsp);
}

Value *llvm::continuationStackOffsetToPtr(IRBuilder<> &B, Value *Offset) {
  assert(Offset->getType()->isIntegerTy(32) &&
         "Stack offset is expected to be an i32");
  Module *M = B.GetInsertPoint()->getModule();
  std::optional<ContStackAddrspace> StackAddrspace =
      DXILContHelper::tryGetStackAddrspace(*M);
  if (!StackAddrspace)
    report_fatal_error("Missing stack addrspace metadata!");
  if (*StackAddrspace == ContStackAddrspace::Scratch)
    return B.CreateIntToPtr(
        Offset,
        B.getInt8Ty()->getPointerTo(static_cast<uint32_t>(*StackAddrspace)));

  // Stack lives in global memory, so add the base address
  assert(*StackAddrspace == ContStackAddrspace::Global &&
         "Unexpected address space of the continuation stack");
  auto *PtrTy = B.getInt8PtrTy(static_cast<uint32_t>(*StackAddrspace));
  Value *BaseAddr = B.CreateCall(getContinuationStackGlobalMemBase(*M));
  BaseAddr = B.CreateIntToPtr(BaseAddr, PtrTy);

  return B.CreateGEP(B.getInt8Ty(), BaseAddr, Offset);
}

Function *llvm::cloneFunctionHeader(Function &F, FunctionType *NewType,
                                    ArrayRef<AttributeSet> ArgAttrs) {
  LLVM_DEBUG(dbgs() << "Cloning function " << F.getName() << " with new type "
                    << *NewType << "\n");
  AttributeList FAttrs = F.getAttributes();
  AttributeList Attributes = AttributeList::get(
      F.getContext(), FAttrs.getFnAttrs(), FAttrs.getRetAttrs(), ArgAttrs);
  Function *NewFunc = Function::Create(NewType, F.getLinkage(), "");
  // Insert new function before F to facilitate writing tests
  F.getParent()->getFunctionList().insert(F.getIterator(), NewFunc);
  NewFunc->setCallingConv(F.getCallingConv());
  NewFunc->setSubprogram(F.getSubprogram());
  NewFunc->setDLLStorageClass(F.getDLLStorageClass());
  NewFunc->setAttributes(Attributes);
  NewFunc->copyMetadata(&F, 0);
  return NewFunc;
}

Function *llvm::cloneFunctionHeaderWithTypes(Function &F,
                                             DXILContFuncTy &NewType,
                                             ArrayRef<AttributeSet> ArgAttrs) {
  FunctionType *FuncTy = NewType.asFunctionType(F.getContext());
  Function *NewFunc = cloneFunctionHeader(F, FuncTy, ArgAttrs);
  NewType.writeMetadata(NewFunc);
  return NewFunc;
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
        LLVM_DEBUG(dbgs() << "Replace " << *Val->getValue()
                          << " in metadata with " << *NewMD << "\n");
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
    if (auto *MD = F.getMetadata(DXILContHelper::MDContinuationName)) {
      if (auto *MDTup = dyn_cast_or_null<MDTuple>(MD))
        Changed |= stripMDCasts(MDTup);
    }
  }

  return Changed;
}

Type *llvm::getContinuationStackOffsetType(LLVMContext &Context) {
  return IntegerType::getInt32Ty(Context);
}

Function *llvm::getContinuationStackOffset(Module &M) {
  StringRef Name = "continuation.getContinuationStackOffset";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::Speculatable, Attribute::WillReturn});
  auto *Func = cast<Function>(
      M.getOrInsertFunction(Name, AL,
                            getContinuationStackOffsetType(C)->getPointerTo())
          .getCallee());
  Func->setDoesNotAccessMemory();
  return Func;
}

Function *llvm::getContinuationStackGlobalMemBase(Module &M) {
  auto *F = M.getFunction("_cont_GetContinuationStackGlobalMemBase");
  assert(F && "Could not find GetContinuationStackGlobalMemBase function");
  assert(F->arg_size() == 0 && F->getReturnType()->isIntegerTy(64));
  return F;
}

bool llvm::isCastGlobal(GlobalValue *Global, Value *V) {
  while (auto *Expr = dyn_cast_or_null<ConstantExpr>(V)) {
    if (Expr->getOpcode() == Instruction::BitCast ||
        Expr->getOpcode() == Instruction::AddrSpaceCast) {
      V = Expr->getOperand(0);
    } else {
      break;
    }
  }
  return Global == V;
}

uint64_t llvm::getInlineHitAttrsBytes(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  auto *GetTriangleHitAttributes =
      M.getFunction("_cont_GetTriangleHitAttributes");
  assert(GetTriangleHitAttributes &&
         "Could not find GetTriangleHitAttributes function");
  auto *InlineHitAttrsTy = GetTriangleHitAttributes->getReturnType();
  uint64_t InlineHitAttrsBytes =
      DL.getTypeStoreSize(InlineHitAttrsTy).getFixedValue();
  assert(
      (InlineHitAttrsBytes % RegisterBytes) == 0 &&
      "Size of inline hit attributes must be a multiple of the register size");
  return InlineHitAttrsBytes;
}

Function *llvm::getRegisterBufferSetPointerBarrier(Module &M) {
  const char *Name = "registerbuffer.setpointerbarrier";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *FuncTy = FunctionType::get(Void, {}, true);
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::WillReturn});
  auto *Func =
      cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
  Func->setOnlyAccessesArgMemory();
  Func->setOnlyWritesMemory();
  return Func;
}

MDTuple *llvm::createRegisterBufferMetadata(LLVMContext &Context,
                                            const RegisterBufferMD &MD) {
  // Metadata format: {i32 registersize, i32 addrspace}
  auto *I32 = Type::getInt32Ty(Context);
  return MDTuple::get(
      Context,
      {ConstantAsMetadata::get(ConstantInt::get(I32, MD.RegisterCount)),
       ConstantAsMetadata::get(ConstantInt::get(I32, MD.Addrspace))});
}

RegisterBufferMD llvm::getRegisterBufferMetadata(const MDNode *MD) {
  const auto *TMD = dyn_cast<MDTuple>(MD);
  assert(TMD && TMD->getNumOperands() == 2 &&
         "registerbuffer metadata must be of format { i32, i32 }");
  const auto *IMD = mdconst::dyn_extract<ConstantInt>(TMD->getOperand(0));
  assert(IMD && IMD->getBitWidth() == 32 &&
         "first registerbuffer metadata must be an i32");
  RegisterBufferMD Data;
  Data.RegisterCount = IMD->getZExtValue();
  IMD = mdconst::dyn_extract<ConstantInt>(TMD->getOperand(1));
  assert(IMD && IMD->getBitWidth() == 32 &&
         "second registerbuffer metadata must be an i32");
  Data.Addrspace = IMD->getZExtValue();
  return Data;
}

Function *llvm::getAccelStructAddr(Module &M, Type *HandleTy) {
  auto *Name = "amd.dx.getAccelStructAddr";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *FuncTy = FunctionType::get(I64, {HandleTy}, false);
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::Speculatable, Attribute::WillReturn});
  auto *Func =
      cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
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

void llvm::analyzeShaderKinds(
    Module &M, MapVector<Function *, DXILShaderKind> &ShaderKinds) {
  auto *EntryPoints = M.getNamedMetadata("dx.entryPoints");
  if (!EntryPoints)
    return;
  for (auto *EntryMD : EntryPoints->operands()) {
    auto *C = mdconst::extract_or_null<Constant>(EntryMD->getOperand(0));
    // Strip bitcasts
    while (auto *Expr = dyn_cast_or_null<ConstantExpr>(C)) {
      if (Expr->getOpcode() == Instruction::BitCast)
        C = Expr->getOperand(0);
      else
        C = nullptr;
    }
    auto *F = extractFunctionOrNull(EntryMD->getOperand(0));
    if (!F)
      continue;
    auto *Props = cast_or_null<MDTuple>(EntryMD->getOperand(4));
    if (!Props)
      continue;

    // Iterate through tag-value pairs
    for (size_t I = 0; I < Props->getNumOperands(); I += 2) {
      auto Tag =
          mdconst::extract<ConstantInt>(Props->getOperand(I))->getZExtValue();
      if (Tag != 8) // kDxilShaderKindTag
        continue;
      auto KindI = mdconst::extract<ConstantInt>(Props->getOperand(I + 1))
                       ->getZExtValue();
      auto Kind = static_cast<DXILShaderKind>(KindI);
      ShaderKinds[F] = Kind;
    }
  }
}

/// Recurse into the first member of the given SystemData to find an object of
/// the wanted type.
Value *llvm::getDXILSystemData(IRBuilder<> &B, Value *SystemData,
                               Type *SystemDataTy, Type *Ty) {
  assert(Ty->isStructTy() && "Expected a struct type for system data");
  LLVM_DEBUG(dbgs() << "Searching for system data type " << *Ty << " in "
                    << *SystemData << " (" << *SystemDataTy << ")\n");
  Type *OrigSystemDataTy = SystemDataTy;
  SmallVector<Value *> Indices;
  // Dereference pointer
  Indices.push_back(B.getInt32(0));

  while (SystemDataTy != Ty) {
    auto *StructTy = dyn_cast<StructType>(SystemDataTy);
    if (!StructTy) {
      LLVM_DEBUG(dbgs() << "System data struct: "; SystemDataTy->dump());
      LLVM_DEBUG(dbgs() << "Wanted struct type: "; Ty->dump());
      errs() << "Invalid system data struct: Did not contain the needed struct "
                "type\n";
      llvm_unreachable("");
    }
    SystemDataTy = StructTy->getElementType(0);
    Indices.push_back(B.getInt32(0));
  }
  if (Indices.size() == 1)
    return SystemData;
  return B.CreateInBoundsGEP(OrigSystemDataTy, SystemData, Indices);
}

CallInst *llvm::replaceIntrinsicCall(IRBuilder<> &B, Type *SystemDataTy,
                                     Value *SystemData, DXILShaderKind Kind,
                                     CallInst *Call) {
  auto &M = *Call->getModule();
  B.SetInsertPoint(Call);

  auto IntrImplEntry = findIntrImplEntryByIntrinsicCall(Call);
  if (IntrImplEntry == std::nullopt)
    return nullptr;

  std::string Name = ("_cont_" + IntrImplEntry->Name).str();
  auto *IntrImpl = DXILContHelper::getAliasedFunction(M, Name);
  if (!IntrImpl)
    cantFail(make_error<StringError>(Twine("Intrinsic implementation '") +
                                         Name + "' not found",
                                     inconvertibleErrorCode()));

  SmallVector<Value *> Arguments;
  // Add the right system data type
  LLVM_DEBUG(dbgs() << "Getting system data for " << Name << "\n");
  Arguments.push_back(getDXILSystemData(B, SystemData, SystemDataTy,
                                        getFuncArgPtrElementType(IntrImpl, 0)));

  // For hit data accessors, get the hit data struct
  if (IntrImplEntry->AccessesHitData) {
    Function *GetHitData;
    if (Kind == DXILShaderKind::AnyHit ||
        Kind == DXILShaderKind::Intersection) {
      auto *GetCandidateState = M.getFunction("_cont_GetCandidateState");
      assert(GetCandidateState && "Could not find GetCandidateState function");
      assert(
          GetCandidateState->getReturnType()->isStructTy() &&
          GetCandidateState->arg_size() == 1
          // Traversal data
          &&
          GetCandidateState->getFunctionType()->getParamType(0)->isPointerTy());
      GetHitData = GetCandidateState;
    } else {
      auto *GetCommittedState = M.getFunction("_cont_GetCommittedState");
      assert(GetCommittedState && "Could not find GetCommittedState function");
      assert(
          GetCommittedState->getReturnType()->isStructTy() &&
          GetCommittedState->arg_size() == 1
          // Traversal data
          &&
          GetCommittedState->getFunctionType()->getParamType(0)->isPointerTy());
      GetHitData = GetCommittedState;
    }
    // The intrinsic expects a pointer, so create an alloca
    auto IP = B.saveIP();
    B.SetInsertPoint(&*Call->getFunction()->begin()->begin());
    auto *HitDataAlloca = B.CreateAlloca(GetHitData->getReturnType());
    B.restoreIP(IP);
    auto *HitData = B.CreateCall(
        GetHitData,
        {getDXILSystemData(B, SystemData, SystemDataTy,
                           getFuncArgPtrElementType(GetHitData, 0))});
    B.CreateStore(HitData, HitDataAlloca);
    Arguments.push_back(HitDataAlloca);
  }

  // Skip the intrinsic id argument, the system data argument and the hit data
  // argument
  auto *IntrType = IntrImpl->getFunctionType();
  for (unsigned CallI = 0, ImplI = IntrImplEntry->AccessesHitData ? 2 : 1,
                ImplE = IntrType->getNumParams();
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
      cantFail(make_error<StringError>(Twine("Can't convert ") + From + " to " +
                                           To + " for intrinsic '" +
                                           IntrImplEntry->Name + "'",
                                       inconvertibleErrorCode()));
    }
  }

  auto *NewCall = B.CreateCall(IntrImpl, Arguments);

  LLVM_DEBUG(dbgs() << "Replacing " << *Call << " by " << *NewCall << "\n");
  if (!Call->getType()->isVoidTy())
    Call->replaceAllUsesWith(NewCall);
  Call->eraseFromParent();
  return NewCall;
}

uint64_t
llvm::computeNeededStackSizeForRegisterBuffer(uint64_t NumI32s,
                                              uint64_t NumReservedRegisters) {
  if (NumI32s <= NumReservedRegisters)
    return 0;

  uint64_t NumStackI32s = NumI32s - NumReservedRegisters;
  return NumStackI32s * RegisterBytes;
}

Type *llvm::getFuncArgPtrElementType(const Function *F, const Argument *Arg) {
  auto *ArgTy = Arg->getType();
  if (!ArgTy->isPointerTy())
    return nullptr;

  // NOTE: fast path code to be removed later
  if (!ArgTy->isOpaquePointerTy())
    return ArgTy->getNonOpaquePointerElementType();

  return DXILContArgTy::get(F, Arg).getPointerElementType();
}

Type *llvm::getFuncArgPtrElementType(const Function *F, int ArgNo) {
  return getFuncArgPtrElementType(F, F->getArg(ArgNo));
}
